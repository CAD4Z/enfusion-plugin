/**
 * The palette and panel command that opens the selected target mod in DayZ Workbench.
 *
 * A mod with no Workbench project of its own, and no workspace project to fall back on, is not a
 * reason to refuse: the command offers to write one — DayZ's own project with the mods' scripts
 * written into its modules — and opens Workbench on it. Nothing is written before the developer
 * has said yes, and what would be written is said first.
 */

import * as vscode from 'vscode';
import { modDefsOf } from '../mods/config';
import { targetsOf } from '../mods/launch/launch';
import { workbenchExecutableOf } from '../mods/machine';
import { mainAddonOf } from '../mods/model';
import { PROJECT_FILE, PROJECT_FOLDER, projectFileOf } from '../mods/workbench';
import { driveLetterOf } from '../mods/workDrive';
import { textOf, writeNew } from '../platform/init';
import { readMachineSettings } from '../platform/machine';
import { startWorkbench } from '../platform/workbench';
import {
  type Discovery,
  type WorkbenchProject,
  findMods,
  projectHomeOf,
  targetSourcesOf,
  workbenchProjectOf,
} from '../platform/workspace';
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
    const executable = workbenchExecutableOf(settings);

    if (target === undefined) {
      await vscode.window.showWarningMessage('Select a launch target before opening Workbench.');
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

    // Asked last, so that a project is only ever written for a Workbench that is then opened. The
    // file watcher sees the new `.gproj`, and the panel's button stops offering to write one.
    const selected =
      workbenchProjectOf(found, target.mod) ??
      (await writeProject(found, target.mod, driveLetterOf(settings.workDriveLetter), log));
    if (selected === undefined) {
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

/**
 * The project the mod has not got, written once the developer has agreed to it. The folders each
 * mod's scripts are in are read out of its own `config.cpp`, so a mod laid out unlike one made here
 * gets a project that finds its scripts all the same.
 */
async function writeProject(
  found: Discovery,
  mod: string,
  drive: string,
  log: vscode.LogOutputChannel,
): Promise<WorkbenchProject | undefined> {
  const home = projectHomeOf(found, mod);
  if (home === undefined) {
    await vscode.window.showWarningMessage(`${mod} is no longer a mod of this workspace.`);
    return undefined;
  }

  const project = vscode.Uri.joinPath(home.folder, PROJECT_FOLDER, PROJECT_FILE);
  const where = vscode.workspace.asRelativePath(project, true);
  const names = home.mods.map((each) => each.name);
  const create = 'Create Project';
  const answer = await vscode.window.showInformationMessage(
    `${mod} has no Workbench project. Create ${where} and open it?`,
    {
      modal: true,
      detail:
        `DayZ's own project, with the work drive ${drive} as its file system and the scripts of ` +
        `${sayList(names)} after the vanilla ones in every module. It is a file of the repository ` +
        'from then on: image sets, defines and the scripts of mods it depends on are added there.',
    },
    create,
  );
  if (answer !== create) {
    return undefined;
  }

  const defs = await Promise.all(
    home.mods.map(async (each) => {
      const config = found.uris.get(mainAddonOf(each)?.config ?? '');
      return modDefsOf(config === undefined ? '' : await textOf(config));
    }),
  );

  try {
    if (!(await writeNew(project, projectFileOf(drive, home.title, defs)))) {
      await vscode.window.showErrorMessage(
        `${where} was not written: there is a file there already, and it is not ours to write over.`,
      );
      return undefined;
    }
  } catch (error: unknown) {
    const message = error instanceof Error ? error.message : String(error);
    log.error(`could not write ${project.fsPath}: ${message}`);
    await vscode.window.showErrorMessage(`Could not write ${where}: ${message}`);
    return undefined;
  }

  log.info(`wrote the Workbench project ${project.fsPath} for ${names.join(', ')}`);

  return { project: project.fsPath, repository: home.repository.fsPath };
}

function sayList(names: readonly string[]): string {
  const last = names.at(-1) ?? '';

  return names.length <= 1 ? last : `${names.slice(0, -1).join(', ')} and ${last}`;
}
