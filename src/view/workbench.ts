/** The palette and panel command that opens the selected target mod in DayZ Workbench. */

import * as vscode from 'vscode';
import { targetsOf } from '../mods/launch';
import { workbenchExecutableOf } from '../mods/machine';
import { startWorkbench } from '../platform/workbench';
import {
  findMods,
  targetSourcesOf,
  workbenchProjectOf,
} from '../platform/workspace';
import { readMachineSettings } from '../platform/machine';
import { platformRefusal, readWorkDrive } from '../platform/workDrive';
import type { Launching } from './launch';

export const WORKBENCH_COMMAND = 'enfusion.workbench.open';

export function registerWorkbenchCommand(
  launching: Launching,
  log: vscode.LogOutputChannel,
): vscode.Disposable {
  return vscode.commands.registerCommand(WORKBENCH_COMMAND, async () => {
    if (!vscode.workspace.isTrusted) {
      await vscode.window.showWarningMessage(
        'Opening Workbench starts an external program with a project from this workspace, so the workspace must be trusted.',
      );
      return;
    }

    const [found, settings] = await Promise.all([findMods(), readMachineSettings()]);
    const drive = await readWorkDrive(settings);
    const target = launching.chosen(targetsOf(targetSourcesOf(found))).target;
    const selected = target === undefined ? undefined : workbenchProjectOf(found, target.mod);
    const executable = workbenchExecutableOf(settings);

    if (target === undefined) {
      await vscode.window.showWarningMessage('Select a launch target before opening Workbench.');
      return;
    }
    if (selected === undefined) {
      await vscode.window.showWarningMessage(`${target.mod} has no .gproj to open in Workbench.`);
      return;
    }
    if (executable === '') {
      await vscode.window.showWarningMessage('DayZ Tools was not found, so Workbench cannot be opened.');
      return;
    }
    const unavailable = platformRefusal();
    if (unavailable !== undefined) {
      await vscode.window.showWarningMessage(unavailable);
      return;
    }
    if (drive.state !== 'mounted') {
      const message =
        drive.at === ''
          ? `${drive.letter} is not mounted, so Workbench cannot read the project resources.`
          : `${drive.letter} is mounted from ${drive.at}, not from ${drive.source}.`;
      await vscode.window.showWarningMessage(message);
      return;
    }

    try {
      await startWorkbench(executable, selected.project, selected.repository);
      log.info(`Workbench: ${selected.project}`);
    } catch (error: unknown) {
      const message = error instanceof Error ? error.message : String(error);
      log.error(`Could not open Workbench: ${message}`);
      await vscode.window.showErrorMessage(`Could not open Workbench: ${message}`);
    }
  });
}
