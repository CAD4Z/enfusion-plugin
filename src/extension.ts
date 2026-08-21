/**
 * Composition root: everything is built here, wired here, and disposed with the extension.
 */

import * as vscode from 'vscode';
import { watchMods } from './platform/workspace';
import { ModsView } from './view/modsView';

export function activate(context: vscode.ExtensionContext): void {
  const log = vscode.window.createOutputChannel('Enfusion', { log: true });
  const view = new ModsView(log);

  context.subscriptions.push(
    log,
    view,
    vscode.window.createTreeView(ModsView.viewId, { treeDataProvider: view }),
    watchMods(() => view.refresh()),
    vscode.commands.registerCommand('enfusion.refresh', () => view.refresh()),
  );
}

export function deactivate(): void {
  // the extension host disposes everything registered in `context.subscriptions`
}
