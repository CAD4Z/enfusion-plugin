/**
 * The Mods panel of the Enfusion container.
 *
 * A webview rather than a tree, because everything the next steps put here — build and run
 * buttons, the state of the work drive — needs more room than a tree item gives. The panel itself
 * holds no state: it is handed the mods and renders them, and every path it sends back is one it
 * was given.
 *
 * The list is read on every change rather than cached: `findFiles` is the editor's own indexed
 * search, and a stale list costs more than the search does.
 */

import { randomBytes } from 'node:crypto';
import * as vscode from 'vscode';
import { SETTING, isWanting } from '../mods/machine';
import type { Mod } from '../mods/model';
import {
  type Link,
  type WorkDrive,
  WORK_DRIVE_ACTIONS,
  isUnlinked,
  refusalOf,
} from '../mods/workDrive';
import { readEnvironment, readMachineSettings } from '../platform/machine';
import { platformRefusal, readLinks, readWorkDrive } from '../platform/workDrive';
import { type Discovery, findMods, prefixesOf } from '../platform/workspace';
import type { ModsMessage, ModView, PanelRequest, WorkDriveView } from '../webview/protocol';
import { BUILD_COMMAND, type BuildTarget } from './build';
import { INIT_COMMAND } from './init';
import { WORK_DRIVE_COMMAND } from './workDrive';

/** A burst of file events — a checkout, a build — should still cost one scan. */
const SETTLE_MS = 200;

export class ModsPanel implements vscode.WebviewViewProvider, vscode.Disposable {
  /** Must match the view id contributed in `package.json`. */
  static readonly viewId = 'enfusion.mods';

  private view: vscode.WebviewView | undefined;
  private settling: NodeJS.Timeout | undefined;
  /** Whether the scan being waited on was asked for by a developer, and so goes to the registry. */
  private rereading = false;
  /** The `Uri` of everything the last scan found, which is what an `open` request resolves through. */
  private uris: ReadonlyMap<string, vscode.Uri> = new Map();
  /** What the current webview holds; a hidden panel is thrown away and resolved again on return. */
  private attached: vscode.Disposable | undefined;

  constructor(
    private readonly extensionUri: vscode.Uri,
    private readonly log: vscode.LogOutputChannel,
  ) {}

  resolveWebviewView(view: vscode.WebviewView): void {
    this.attached?.dispose();
    this.view = view;
    view.webview.options = {
      enableScripts: true,
      localResourceRoots: [vscode.Uri.joinPath(this.extensionUri, 'dist')],
    };

    // Listening before the page exists, because the page asks for the mods as soon as it loads.
    this.attached = vscode.Disposable.from(
      view.webview.onDidReceiveMessage((request: PanelRequest) => {
        this.receive(request);
      }),
      view.onDidDispose(() => {
        this.view = undefined;
      }),
    );

    view.webview.html = this.page(view.webview);
  }

  /** Coalesces the file events of one edit, one checkout, one build into a single scan. */
  refresh(): void {
    this.schedule();
  }

  /**
   * The refresh a developer asked for, which goes back to the registry as well as to the disk —
   * an installation that appeared while the editor was open is the case this exists for.
   */
  reread(): void {
    this.rereading = true;
    this.schedule();
  }

  /** A scan asked for while one is settling joins it, and a reread is not lost to a file event. */
  private schedule(): void {
    clearTimeout(this.settling);
    this.settling = setTimeout(() => {
      const reread = this.rereading;
      this.rereading = false;
      this.report(this.send(reread));
    }, SETTLE_MS);
  }

  dispose(): void {
    clearTimeout(this.settling);
    this.attached?.dispose();
  }

  private receive(request: PanelRequest): void {
    switch (request.type) {
      // The panel is rebuilt every time it becomes visible, and asks for the mods when it is.
      case 'ready':
        this.report(this.send(false));
        return;
      case 'refresh':
        this.reread();
        return;
      case 'open':
        this.report(this.open(request.path, request.line, request.column));
        return;
      case 'settings':
        this.report(openSettings(request.id));
        return;
      // Through the command, so that the button and the palette do the very same thing.
      case 'workDrive':
        this.report(runCommand(WORK_DRIVE_COMMAND[request.action]));
        return;
      case 'build':
        this.report(
          runCommand(BUILD_COMMAND.addon, { mod: request.mod, addon: request.addon }),
        );
        return;
      case 'init':
        this.report(runCommand(INIT_COMMAND.mod));
        return;
      case 'adopt':
        this.report(runCommand(INIT_COMMAND.adopt, { mod: request.mod }));
        return;
      case 'addon':
        this.report(runCommand(INIT_COMMAND.addon, { mod: request.mod }));
        return;
    }
  }

  /** A scan or an open that failed is worth a line in the log, not a popup over the editor. */
  private report(work: Promise<void>): void {
    work.catch((error: unknown) => {
      this.log.error(error instanceof Error ? error : String(error));
    });
  }

  private async send(reread: boolean): Promise<void> {
    const view = this.view;
    if (view === undefined) {
      return;
    }

    const [found, settings] = await Promise.all([findMods(), readMachineSettings(reread)]);
    const [entries, drive] = await Promise.all([
      readEnvironment(settings),
      readWorkDrive(settings),
    ]);
    const links = await readLinks(drive, prefixesOf(found));

    this.uris = found.uris;
    this.log.info(`${found.mods.length} mod(s): ${found.mods.map((mod) => mod.name).join(', ')}`);
    this.log.info(
      `environment: ${entries.map((entry) => `${entry.kind} ${entry.state}`).join(', ')}`,
    );
    this.log.info(
      `work drive: ${drive.letter} ${drive.state}${drive.at === '' ? '' : ` at ${drive.at}`}, ` +
        `${links.filter(isUnlinked).length} of ${links.length} mod(s) not linked`,
    );

    const message: ModsMessage = {
      type: 'mods',
      environment: { entries, wanting: entries.filter(isWanting).length },
      workDrive: workDriveViewOf(drive, links),
      workspaces: [...found.workspaces].map(([path, problems]) => ({
        path,
        location: locationOfFile(path, found),
        owns: ownedBy(path, found),
        problems,
      })),
      mods: found.mods.map((mod) => toView(mod, found, links)),
    };
    await view.webview.postMessage(message);
  }

  /** At the place the panel named, when it named one: a problem is worth landing on. */
  private async open(path: string, line?: number, column?: number): Promise<void> {
    const uri = this.uris.get(path);
    if (uri === undefined) {
      return;
    }

    if (line === undefined) {
      await vscode.window.showTextDocument(uri);
      return;
    }

    const at = new vscode.Position(line - 1, (column ?? 1) - 1);
    await vscode.window.showTextDocument(uri, { selection: new vscode.Range(at, at) });
  }

  private page(webview: vscode.Webview): string {
    const script = this.asset(webview, 'webview.js');
    const style = this.asset(webview, 'webview.css');
    const nonce = randomBytes(16).toString('base64');

    return `<!DOCTYPE html>
<html lang="en">
  <head>
    <meta charset="UTF-8" />
    <meta
      http-equiv="Content-Security-Policy"
      content="default-src 'none'; style-src ${webview.cspSource} 'unsafe-inline'; script-src 'nonce-${nonce}';"
    />
    <meta name="viewport" content="width=device-width, initial-scale=1.0" />
    <link href="${style}" rel="stylesheet" />
    <title>Mods</title>
  </head>
  <body>
    <script type="module" nonce="${nonce}" src="${script}"></script>
  </body>
</html>`;
  }

  private asset(webview: vscode.Webview, file: string): string {
    return webview.asWebviewUri(vscode.Uri.joinPath(this.extensionUri, 'dist', file)).toString();
  }
}

/** The one place a setting is filled in from, which is where a missing one sends the developer. */
async function openSettings(id: string): Promise<void> {
  await vscode.commands.executeCommand('workbench.action.openSettings', id);
}

async function runCommand(
  command: string,
  argument?: BuildTarget | { mod: string },
): Promise<void> {
  await vscode.commands.executeCommand(command, argument);
}

/**
 * The work drive as the panel shows it. Every button carries the reason it would refuse, so the
 * one that is disabled says why without being pressed; the warning is the same sentence, because
 * "mounted from the wrong folder" is one fact and deserves one wording.
 */
function workDriveViewOf(drive: WorkDrive, links: readonly Link[]): WorkDriveView {
  const platform = platformRefusal();

  return {
    letter: drive.letter,
    source: drive.source,
    at: drive.at,
    state: drive.state,
    setting: SETTING.workDrive,
    warning: platform ?? (drive.state === 'elsewhere' ? refusalOf(drive, 'link') : undefined),
    actions: WORK_DRIVE_ACTIONS.map((action) => ({
      action,
      refusal: platform ?? refusalOf(drive, action),
    })),
    unlinked: links.filter(isUnlinked).length,
  };
}

function toView(mod: Mod, found: Discovery, links: readonly Link[]): ModView {
  const configured = mod.manifest === undefined ? undefined : found.configured.get(mod.manifest);
  const manifest = configured?.configuration.manifest;
  const link = links.find((made) => made.prefixRoot === mod.prefixRoot);

  return {
    name: mod.name,
    location: locationOfMod(mod, found),
    title: manifest?.name,
    description: manifest?.description,
    manifest: mod.manifest,
    manifestProblems: configured?.problems ?? [],
    link: link && { state: link.state, path: link.path, at: link.at },
    addons: mod.addons.map((addon) => ({
      name: addon.name,
      main: addon.main,
      config: addon.config,
      patches: addon.patches,
      unresolved: addon.unresolved,
    })),
    problems: mod.problems,
  };
}

/**
 * The mod root has no `Uri` of its own — only files were searched for — so it borrows one from a
 * file inside it. Rebuilding a `Uri` from the path instead would assume the workspace is on disk.
 */
function locationOfMod(mod: Mod, found: Discovery): string {
  const anchor = found.uris.get(mod.manifest ?? mod.addons[0]?.config ?? '');
  return anchor ? vscode.workspace.asRelativePath(anchor.with({ path: mod.root }), true) : mod.root;
}

function locationOfFile(path: string, found: Discovery): string {
  const uri = found.uris.get(path);
  return uri ? vscode.workspace.asRelativePath(uri, true) : path;
}

/** The mods this workspace file actually owns the launch of, which is what it is labelled by. */
function ownedBy(path: string, found: Discovery): string[] {
  return found.mods.flatMap((mod) => {
    const manifest = mod.manifest;
    const owner = manifest === undefined ? undefined : found.configured.get(manifest)?.workspace;

    return owner === path ? [mod.name] : [];
  });
}
