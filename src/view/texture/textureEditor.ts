/**
 * A standalone, read-only custom editor over one local EDDS file.
 *
 * The document owns only its URI. No sibling is listed and no workspace is consulted: inspection
 * and selected-mip decoding both go to the bundled converter with this exact path.
 */

import { randomBytes } from 'node:crypto';
import * as vscode from 'vscode';
import {
  type TextureEditorEffect,
  type TextureEditorEvent,
  type TextureEditorState,
  openedTexture,
  updateTexture,
} from '../../mods/texture/textureEditor';
import { literalGlobOf, nameOf } from '../../mods/paths';
import { EddsConverter } from '../../platform/texture/eddsConverter';
import { reconvertSourceOf } from '../../platform/texture/textureConversion';
import type { TextureRequest, TextureStateMessage } from '../../webview/texture/textureProtocol';

class EddsDocument implements vscode.CustomDocument {
  constructor(readonly uri: vscode.Uri) {}

  dispose(): void {
    // The document owns no handle: the converter opens the selected file only for each operation.
  }
}

export class TextureEditor implements vscode.CustomReadonlyEditorProvider<EddsDocument> {
  /** Must match the view type contributed in package.json. */
  static readonly viewType = 'enfusion.eddsPreview';

  constructor(
    private readonly extensionUri: vscode.Uri,
    private readonly converter: EddsConverter,
    private readonly log: vscode.LogOutputChannel,
  ) {}

  openCustomDocument(
    uri: vscode.Uri,
    _context: vscode.CustomDocumentOpenContext,
    _token: vscode.CancellationToken,
  ): EddsDocument {
    return new EddsDocument(uri);
  }

  resolveCustomEditor(
    document: EddsDocument,
    panel: vscode.WebviewPanel,
    token: vscode.CancellationToken,
  ): void {
    panel.webview.options = {
      enableScripts: true,
      localResourceRoots: [vscode.Uri.joinPath(this.extensionUri, 'dist')],
    };
    panel.webview.html = this.page(panel.webview);

    const opening = openedTexture();
    let current: TextureEditorState = opening.state;
    let started = false;
    let disposed = false;
    const work = new Set<AbortController>();
    let preview: AbortController | undefined;

    const send = (): void => {
      if (!disposed) {
        const message: TextureStateMessage = { type: 'state', state: current };
        void panel.webview.postMessage(message);
      }
    };

    const apply = (event: TextureEditorEvent): void => {
      if (disposed) {
        return;
      }
      const update = updateTexture(current, event);
      current = update.state;
      send();
      for (const effect of update.effects) {
        run(effect);
      }
    };

    const run = (effect: TextureEditorEffect): void => {
      if (effect.kind === 'preview') {
        preview?.abort();
      }
      const controller = new AbortController();
      work.add(controller);
      if (effect.kind === 'preview') {
        preview = controller;
      }

      const operation =
        effect.kind === 'inspect'
          ? this.converter.inspect(document.uri.fsPath, controller.signal).then((inspection) => {
              apply({ kind: 'inspected', inspection });
            })
          : this.converter
              .preview(document.uri.fsPath, effect.mip, controller.signal)
              .then((decoded) => {
                apply({ kind: 'previewed', request: effect.request, preview: decoded });
              });

      operation
        .catch((error: unknown) => {
          if (controller.signal.aborted || disposed) {
            return;
          }
          const reason = error instanceof Error ? error.message : String(error);
          this.log.error(`EDDS preview: ${reason}`);
          apply(
            effect.kind === 'inspect'
              ? { kind: 'inspection-failed', reason }
              : { kind: 'preview-failed', request: effect.request, reason },
          );
        })
        .finally(() => {
          work.delete(controller);
          if (preview === controller) {
            preview = undefined;
          }
        });
    };

    // Inspection, the selected mip and the reconversion offer, all read afresh from the file.
    const start = (effects: readonly TextureEditorEffect[]): void => {
      for (const effect of effects) {
        run(effect);
      }
      void reconvertSourceOf(document.uri, this.converter).then((reconvert) => {
        apply(
          reconvert.kind === 'available'
            ? { kind: 'reconversion-available', source: reconvert.source }
            : { kind: 'reconversion-refused', reason: reconvert.reason },
        );
      });
    };

    // A Convert, a Reconvert or a batch rewrites the file under an open preview, which would
    // otherwise go on showing the header and the pixels it read first. The pair is published as
    // a short run of renames, so the preview starts over once they have settled.
    let settling: NodeJS.Timeout | undefined;
    const restart = (): void => {
      if (disposed || !started) {
        return;
      }
      for (const controller of work) {
        controller.abort();
      }
      const reopened = openedTexture();
      current = reopened.state;
      send();
      start(reopened.effects);
    };
    const changed = (): void => {
      if (settling !== undefined) {
        clearTimeout(settling);
      }
      settling = setTimeout(restart, SETTLE_MS);
    };
    const watcher = document.uri.scheme === 'file'
      ? vscode.workspace.createFileSystemWatcher(
          new vscode.RelativePattern(vscode.Uri.joinPath(document.uri, '..'), literalGlobOf(nameOf(document.uri.path))),
          true,
          false,
          false,
        )
      : undefined;

    const attached = vscode.Disposable.from(
      ...(watcher === undefined ? [] : [watcher, watcher.onDidChange(changed), watcher.onDidDelete(changed)]),
      panel.webview.onDidReceiveMessage((request: TextureRequest) => {
        switch (request.type) {
          case 'ready':
            send();
            if (!started) {
              started = true;
              if (document.uri.scheme !== 'file') {
                apply({ kind: 'inspection-failed', reason: 'EDDS preview opens local files only.' });
              } else if (process.platform !== 'win32') {
                apply({
                  kind: 'inspection-failed',
                  reason: 'EDDS preview is currently available on Windows x64.',
                });
              } else {
                start(opening.effects);
              }
            }
            return;
          case 'select-channel':
            apply({ kind: 'select-channel', channel: request.channel });
            return;
          case 'select-mip':
            apply({ kind: 'select-mip', mip: request.mip });
            return;
          case 'reconvert':
            if (current.reconvert.kind === 'available') {
              void vscode.commands.executeCommand(
                'vscode.openWith',
                vscode.Uri.file(current.reconvert.source),
                'enfusion.textureConversion',
              );
            }
            return;
        }
      }),
      token.onCancellationRequested(() => {
        for (const controller of work) {
          controller.abort();
        }
      }),
    );

    panel.onDidDispose(() => {
      disposed = true;
      if (settling !== undefined) {
        clearTimeout(settling);
      }
      attached.dispose();
      for (const controller of work) {
        controller.abort();
      }
      work.clear();
    });
  }

  private page(webview: vscode.Webview): string {
    const script = this.asset(webview, 'texture.js');
    const style = this.asset(webview, 'texture.css');
    const nonce = randomBytes(16).toString('base64');

    return `<!DOCTYPE html>
<html lang="en">
  <head>
    <meta charset="UTF-8" />
    <meta
      http-equiv="Content-Security-Policy"
      content="default-src 'none'; style-src ${webview.cspSource}; script-src 'nonce-${nonce}';"
    />
    <meta name="viewport" content="width=device-width, initial-scale=1.0" />
    <link href="${style}" rel="stylesheet" />
    <title>EDDS Preview</title>
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

export function registerTextureEditor(
  context: vscode.ExtensionContext,
  log: vscode.LogOutputChannel,
  converter = new EddsConverter(context.extensionPath),
): vscode.Disposable {
  const provider = new TextureEditor(context.extensionUri, converter, log);
  const editor = vscode.window.registerCustomEditorProvider(TextureEditor.viewType, provider, {
    supportsMultipleEditorsPerDocument: true,
  });
  const command = vscode.commands.registerCommand(
    'enfusion.edds.openPreview',
    async (selected?: vscode.Uri) => {
      if (process.platform !== 'win32') {
        await vscode.window.showErrorMessage('EDDS preview is currently available on Windows x64.');
        return;
      }
      const uri = selected ?? (await chooseEdds());
      if (uri === undefined) {
        return;
      }
      if (uri.scheme !== 'file') {
        await vscode.window.showErrorMessage('EDDS preview opens local files only.');
        return;
      }
      await vscode.commands.executeCommand('vscode.openWith', uri, TextureEditor.viewType);
    },
  );

  return vscode.Disposable.from(editor, command);
}

/** How long the file must stay unchanged before the preview reads it again. */
const SETTLE_MS = 300;

async function chooseEdds(): Promise<vscode.Uri | undefined> {
  const selected = await vscode.window.showOpenDialog({
    canSelectFiles: true,
    canSelectFolders: false,
    canSelectMany: false,
    filters: { 'Enfusion runtime texture': ['edds'] },
    title: 'Open EDDS Preview',
  });
  return selected?.[0];
}
