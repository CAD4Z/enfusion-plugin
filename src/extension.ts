/**
 * Composition root: everything is built here, wired here, and disposed with the extension.
 */

import * as vscode from 'vscode';
import { watchMods } from './platform/workspace';
import { ModsPanel } from './view/modsPanel';

export function activate(context: vscode.ExtensionContext): void {
  const log = vscode.window.createOutputChannel('Enfusion', { log: true });
  const panel = new ModsPanel(context.extensionUri, log);

  context.subscriptions.push(
    log,
    panel,
    vscode.window.registerWebviewViewProvider(ModsPanel.viewId, panel),
    watchMods(() => {
      panel.refresh();
    }),
    vscode.commands.registerCommand('enfusion.refresh', () => {
      panel.refresh();
    }),
  );
}

export function deactivate(): void {
  // the extension host disposes everything registered in `context.subscriptions`
}
