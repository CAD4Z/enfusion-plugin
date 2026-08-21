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
import type { Mod } from '../mods/model';
import { type Discovery, findMods } from '../platform/workspace';
import type { ModsMessage, ModView, PanelRequest } from '../webview/protocol';

/** A burst of file events — a checkout, a build — should still cost one scan. */
const SETTLE_MS = 200;

export class ModsPanel implements vscode.WebviewViewProvider, vscode.Disposable {
  /** Must match the view id contributed in `package.json`. */
  static readonly viewId = 'enfusion.mods';

  private view: vscode.WebviewView | undefined;
  private settling: NodeJS.Timeout | undefined;
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
    clearTimeout(this.settling);
    this.settling = setTimeout(() => this.report(this.send()), SETTLE_MS);
  }

  dispose(): void {
    clearTimeout(this.settling);
    this.attached?.dispose();
  }

  private receive(request: PanelRequest): void {
    switch (request.type) {
      // The panel is rebuilt every time it becomes visible, and asks for the mods when it is.
      case 'ready':
      case 'refresh':
        this.report(this.send());
        return;
      case 'open':
        this.report(this.open(request.path));
        return;
    }
  }

  /** A scan or an open that failed is worth a line in the log, not a popup over the editor. */
  private report(work: Promise<void>): void {
    work.catch((error: unknown) => {
      this.log.error(error instanceof Error ? error : String(error));
    });
  }

  private async send(): Promise<void> {
    const view = this.view;
    if (view === undefined) {
      return;
    }

    const found = await findMods();
    this.uris = found.uris;
    this.log.info(`${found.mods.length} mod(s): ${found.mods.map((mod) => mod.name).join(', ')}`);

    const message: ModsMessage = {
      type: 'mods',
      mods: found.mods.map((mod) => toView(mod, found)),
    };
    await view.webview.postMessage(message);
  }

  private async open(path: string): Promise<void> {
    const uri = this.uris.get(path);
    if (uri) {
      await vscode.window.showTextDocument(uri);
    }
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

function toView(mod: Mod, found: Discovery): ModView {
  return {
    name: mod.name,
    location: locationOf(mod, found),
    configured: mod.manifest !== undefined,
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
function locationOf(mod: Mod, found: Discovery): string {
  const anchor = found.uris.get(mod.manifest ?? mod.addons[0]?.config ?? '');
  return anchor
    ? vscode.workspace.asRelativePath(anchor.with({ path: mod.root }), true)
    : mod.root;
}
