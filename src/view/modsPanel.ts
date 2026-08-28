/**
 * The Mods panel of the Enfusion container.
 *
 * A webview rather than a tree, because a row of buttons over a list of mods is not something a
 * tree item gives room for. The panel itself holds no state: it is handed the mods, the buttons
 * and the words for both, and every path it sends back is one it was given.
 *
 * The list is read on every change rather than cached: `findFiles` is the editor's own indexed
 * search, and a stale list costs more than the search does.
 */

import { randomBytes } from 'node:crypto';
import * as vscode from 'vscode';
import { targetsOf } from '../mods/launch';
import { isWanting } from '../mods/machine';
import { MANIFEST_FILE, type Mod } from '../mods/model';
import {
  type Link,
  type WorkDrive,
  type WorkDriveAction,
  WORK_DRIVE_ACTIONS,
  isUnlinked,
  refusalOf,
} from '../mods/workDrive';
import { readEnvironment, readMachineSettings } from '../platform/machine';
import { platformRefusal, readLinks, readWorkDrive } from '../platform/workDrive';
import {
  type Discovery,
  findMods,
  prefixesOf,
  targetSourcesOf,
} from '../platform/workspace';
import type { ModView, ModsMessage, PanelRequest, ToolsView } from '../webview/protocol';
import { BUILD_COMMAND, type BuildTarget } from './build';
import { INIT_COMMAND } from './init';
import { LAUNCH_COMMAND } from './launch';
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
      // Asked for by a page that found the extension behind it to be of another version, which
      // is what installing over a running one leaves until the window is restarted.
      case 'reload':
        this.report(runCommand('workbench.action.reloadWindow'));
        return;
      case 'open':
        this.report(this.open(request.path, request.line, request.column));
        return;
      // Through the commands, so that the buttons and the palette do the very same things.
      case 'launch':
        this.report(runCommand(LAUNCH_COMMAND.start));
        return;
      case 'workDrive':
        this.report(runCommand(WORK_DRIVE_COMMAND[request.action]));
        return;
      case 'build':
        this.report(runCommand(BUILD_COMMAND.addon, { mod: request.mod, addon: request.addon }));
        return;
      case 'buildAll':
        this.report(runCommand(BUILD_COMMAND.all));
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
    // The panel shows none of this any more, so the log is where a machine that answered for
    // nothing is seen at all — before a build refuses over it rather than during.
    const wanting = entries.filter(isWanting);
    this.log.info(
      `environment: ${entries.map((entry) => `${entry.kind} ${entry.state}`).join(', ')}` +
        `${wanting.length === 0 ? ' — ready' : ` — ${wanting.length} to set`}`,
    );
    this.log.info(
      `work drive: ${drive.letter} ${drive.state}${drive.at === '' ? '' : ` at ${drive.at}`}, ` +
        `${links.filter(isUnlinked).length} of ${links.length} mod(s) not linked`,
    );

    const message: ModsMessage = {
      type: 'mods',
      tools: toolsOf(drive, found),
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

    // Through the editor's own opening, so that a `.enf` opens the way it opens everywhere else —
    // as the form, unless the developer has told the editor otherwise. A place to land on is a
    // text thing, though, and no form has a line 12 to put a cursor on.
    if (line === undefined) {
      await vscode.commands.executeCommand('vscode.open', uri);
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

async function runCommand(
  command: string,
  argument?: BuildTarget | { mod: string },
): Promise<void> {
  await vscode.commands.executeCommand(command, argument);
}

/** What each work drive button does, said the way the command that does it would say it. */
function workDriveTitle(action: WorkDriveAction, drive: WorkDrive): string {
  switch (action) {
    case 'mount':
      return drive.source === ''
        ? `Put the folder the settings name up under ${drive.letter}`
        : `Put ${drive.source} up under ${drive.letter}`;
    case 'unmount':
      return `Take ${drive.letter} down and free the letter`;
    case 'link':
      return `Put the prefix root of every mod of this workspace onto ${drive.letter}`;
  }
}

/**
 * The row of buttons, with the reason each one would refuse already on it: the disabled button
 * says why without being pressed, and it says it in the same words the command would.
 */
function toolsOf(drive: WorkDrive, found: Discovery): ToolsView {
  const platform = platformRefusal();
  const targets = targetsOf(targetSourcesOf(found));
  const addons = found.mods.reduce((count, mod) => count + mod.addons.length, 0);

  return {
    start: {
      title: 'Put the game up: the launch target chosen on the status bar',
      refusal:
        targets.length === 0
          ? `Nothing to launch: no "targets" in the ${MANIFEST_FILE} of this workspace.`
          : undefined,
    },
    build: {
      title: 'Pack every addon of this workspace into its pbo, in dependency order',
      refusal: addons === 0 ? 'No addon of this workspace can be built.' : undefined,
    },
    workDrive: WORK_DRIVE_ACTIONS.map((action) => ({
      action,
      title: workDriveTitle(action, drive),
      refusal: platform ?? refusalOf(drive, action),
    })),
  };
}

function toView(mod: Mod, found: Discovery, links: readonly Link[]): ModView {
  const configured = mod.manifest === undefined ? undefined : found.configured.get(mod.manifest);
  const link = links.find((made) => made.prefixRoot === mod.prefixRoot);

  return {
    name: mod.name,
    manifest: mod.manifest,
    manifestProblems: configured?.problems ?? [],
    link: link && { state: link.state, path: link.path, at: link.at },
    addons: mod.addons.map((addon) => ({
      name: addon.name,
      main: addon.main,
      config: addon.config,
      patches: addon.patches,
    })),
    problems: mod.problems,
  };
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
