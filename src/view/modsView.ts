/**
 * The Mods tree of the Enfusion container.
 *
 * The list is read on every expansion rather than cached: `findFiles` is the editor's own indexed
 * search, and a stale tree costs more than the search does. An empty result is left empty on
 * purpose — the `viewsWelcome` contribution in `package.json` is what fills it.
 */

import * as vscode from 'vscode';
import { findMods, type ModEntry } from '../platform/workspace';

export class ModsView implements vscode.TreeDataProvider<ModEntry>, vscode.Disposable {
  /** Must match the view id contributed in `package.json`. */
  static readonly viewId = 'enfusion.mods';

  private readonly changed = new vscode.EventEmitter<void>();
  readonly onDidChangeTreeData = this.changed.event;

  constructor(private readonly log: vscode.LogOutputChannel) {}

  async getChildren(element?: ModEntry): Promise<ModEntry[]> {
    // Mods are the only level for now, so anything below one is empty.
    if (element) {
      return [];
    }

    const entries = await findMods();
    this.log.info(`${entries.length} mod(s): ${entries.map((entry) => entry.mod.name).join(', ')}`);
    return entries;
  }

  getTreeItem(entry: ModEntry): vscode.TreeItem {
    const item = new vscode.TreeItem(entry.mod.name, vscode.TreeItemCollapsibleState.None);

    item.description = vscode.workspace.asRelativePath(entry.config, true);
    item.tooltip = entry.config.fsPath;
    item.iconPath = new vscode.ThemeIcon('package');
    item.contextValue = 'enfusion.mod';
    item.resourceUri = entry.config;
    item.command = {
      command: 'vscode.open',
      title: 'Open config.cpp',
      arguments: [entry.config],
    };

    return item;
  }

  refresh(): void {
    this.changed.fire();
  }

  dispose(): void {
    this.changed.dispose();
  }
}
