/**
 * The `.enf` editor: a form over `mod.enf` and `workspace.enf`.
 *
 * A `CustomTextEditorProvider` rather than an editor of its own, which is the whole point — the
 * thing being edited stays a `TextDocument`. The file is text under git, and everything the editor
 * already gives a text file goes on working: the undo stack, the dirty mark, Ctrl+S, the diff, the
 * merge. The form is a second view of that document, not a second copy of it.
 *
 * So both directions are the document's. An edit from the form goes back as a `WorkspaceEdit` on
 * the very span the domain worked out, and a change to the document — from the text editor, from a
 * revert, from a checkout — comes back here and rebuilds the form. Neither side keeps a copy to
 * fall behind with.
 *
 * The one thing looked up here rather than read out of the document is which `workspace.enf` owns
 * this mod's launch block. The domain still decides it — `workspaceFor`, the same rule the panel
 * goes by — but only the workspace can say which files there are to decide between. The form says
 * it because a block silently ignored is worse than a block that is not there.
 */

import { randomBytes } from 'node:crypto';
import * as vscode from 'vscode';
import { type FormEdit, type ManifestKind, changesOf, formOf } from '../mods/form';
import { MANIFEST_FILE } from '../mods/model';
import { folderOf, nameOf } from '../mods/paths';
import { launchOwnerOf } from '../platform/workspace';
import type { FormRequest, ManifestMessage } from '../webview/formProtocol';

export class EnfEditor implements vscode.CustomTextEditorProvider {
  /** Must match the view type contributed in `package.json`. */
  static readonly viewType = 'enfusion.enf';

  constructor(
    private readonly extensionUri: vscode.Uri,
    private readonly log: vscode.LogOutputChannel,
  ) {}

  resolveCustomTextEditor(document: vscode.TextDocument, panel: vscode.WebviewPanel): void {
    const kind = kindOf(document);
    panel.webview.options = {
      enableScripts: true,
      localResourceRoots: [vscode.Uri.joinPath(this.extensionUri, 'dist')],
    };

    // Which file owns the launch block is a fact about the workspace rather than about this file,
    // so it is looked up rather than read — and looked up again on every `ready`, which is every
    // time the form is built: a `workspace.enf` may have appeared above the mod since the last one.
    let ownedBy: string | undefined;

    const send = (): void => {
      void panel.webview.postMessage(messageOf(document, kind, ownedBy));
    };

    const reread = async (): Promise<void> => {
      ownedBy = kind === 'mod' ? await launchOwnerOf(folderOf(document.uri.path)) : undefined;
      send();
    };

    // Listening before the page exists, because the page asks for the manifest as soon as it loads.
    const attached = vscode.Disposable.from(
      panel.webview.onDidReceiveMessage((request: FormRequest) => {
        switch (request.type) {
          // The form is built from scratch every time it becomes visible, and asks when it is.
          case 'ready':
            this.report(reread());
            return;
          case 'edit':
            this.report(apply(document, kind, request.edit));
            return;
          case 'text':
            this.report(asText(document, request.line, request.column));
            return;
        }
      }),
      // The same document edited as text, reverted, or checked out under the editor: the form is
      // a view of it, so it is rebuilt rather than asked to agree.
      vscode.workspace.onDidChangeTextDocument((event) => {
        if (event.document.uri.toString() === document.uri.toString()) {
          send();
        }
      }),
    );

    panel.onDidDispose(() => {
      attached.dispose();
    });

    panel.webview.html = this.page(panel.webview);
  }

  /** An edit or an open that failed is worth a line in the log, not a popup over the editor. */
  private report(work: Promise<void>): void {
    work.catch((error: unknown) => {
      this.log.error(error instanceof Error ? error : String(error));
    });
  }

  private page(webview: vscode.Webview): string {
    const script = this.asset(webview, 'form.js');
    const style = this.asset(webview, 'form.css');
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
    <title>Manifest</title>
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

/** Which of the two files this is, which the editor is only ever opened on one of. */
function kindOf(document: vscode.TextDocument): ManifestKind {
  return nameOf(document.uri.path) === MANIFEST_FILE ? 'mod' : 'workspace';
}

function messageOf(
  document: vscode.TextDocument,
  kind: ManifestKind,
  ownedBy: string | undefined,
): ManifestMessage {
  const form = formOf(kind, document.getText());

  return {
    type: 'manifest',
    kind,
    file: nameOf(document.uri.path),
    mod: form.mod,
    launch: form.launch,
    problems: form.problems,
    refusal: form.refusal,
    ownedBy,
  };
}

/**
 * One move of the form as one edit of the document. It goes through `applyEdit` rather than
 * through the file system, so it lands on the undo stack and in the dirty mark: the developer
 * saves the file, and until they do nothing has been written anywhere.
 */
async function apply(
  document: vscode.TextDocument,
  kind: ManifestKind,
  edit: FormEdit,
): Promise<void> {
  const changes = changesOf(kind, document.getText(), edit);
  if (changes.length === 0) {
    return;
  }

  const work = new vscode.WorkspaceEdit();
  for (const change of changes) {
    work.replace(
      document.uri,
      new vscode.Range(
        document.positionAt(change.offset),
        document.positionAt(change.offset + change.length),
      ),
      change.content,
    );
  }

  await vscode.workspace.applyEdit(work);
}

/** The same document in the text editor, at the place the form named when it named one. */
async function asText(
  document: vscode.TextDocument,
  line?: number,
  column?: number,
): Promise<void> {
  const editor = await vscode.window.showTextDocument(document, { preview: false });
  if (line === undefined) {
    return;
  }

  const at = new vscode.Position(line - 1, (column ?? 1) - 1);
  editor.selection = new vscode.Selection(at, at);
  editor.revealRange(new vscode.Range(at, at));
}
