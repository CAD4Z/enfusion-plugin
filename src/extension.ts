/**
 * Composition root: everything is built here, wired here, and disposed with the extension.
 */

import * as vscode from 'vscode';
import { watchMachineSettings } from './platform/machine';
import { watchMods } from './platform/workspace';
import { registerBuildCommands } from './view/build';
import { EnfEditor } from './view/enfEditor';
import { registerInitCommands } from './view/init';
import { registerLaunch } from './view/launch';
import { ModsPanel } from './view/modsPanel';
import { registerWorkDriveCommands } from './view/workDrive';

export function activate(context: vscode.ExtensionContext): void {
  const log = vscode.window.createOutputChannel('Enfusion', { log: true });

  // Which version is actually running, said once at the top of the log. Installing over a running
  // extension leaves the old code in the host until the window is reloaded, and every symptom of
  // that looks like a bug in the new version: a setting that is ignored, a panel that stays blank.
  // The log is the only place that can answer it, so it answers it without being asked.
  log.info(`Enfusion ${versionOf(context)} activated`);

  const panel = new ModsPanel(context.extensionUri, log);
  // The chosen target is the workspace's rather than the machine's: it names a target of this
  // workspace's `.enf`, and means nothing in another one.
  const launching = registerLaunch(context.workspaceState, log);

  context.subscriptions.push(
    log,
    panel,
    launching,
    vscode.window.registerWebviewViewProvider(ModsPanel.viewId, panel),
    // The form over a `.enf`. A second view of the very same document, so the text editor is
    // still there — under Reopen Editor With, and under the button the form itself carries.
    vscode.window.registerCustomEditorProvider(
      EnfEditor.viewType,
      new EnfEditor(context.extensionUri, log),
    ),
    watchMods(() => {
      panel.refresh();
      // A target added to a `.enf` shows on the status bar without anything being pressed.
      launching.refresh();
    }),
    // The panel shows what the machine resolved to, so a setting changing is a change to show.
    watchMachineSettings(() => {
      panel.refresh();
    }),
    vscode.commands.registerCommand('enfusion.refresh', () => {
      panel.reread();
    }),
    // Mounting and linking change nothing a file watcher would notice, so they say so themselves.
    registerWorkDriveCommands(log, () => {
      panel.refresh();
    }),
    // A build writes outside the workspace, so nothing about the panel changes when one finishes.
    registerBuildCommands(log),
    // Making a mod does change the workspace, and the watcher will say so — but linking the new
    // mod onto the work drive is a change nothing watches, so these say so themselves.
    registerInitCommands(log, () => {
      panel.refresh();
    }),
  );
}

/** Out of the manifest the editor loaded, which is the one that is running rather than the one on disk. */
function versionOf(context: vscode.ExtensionContext): string {
  const version: unknown = (context.extension.packageJSON as { version?: unknown }).version;

  return typeof version === 'string' ? version : 'of an unknown version';
}

export function deactivate(): void {
  // the extension host disposes everything registered in `context.subscriptions`
}
