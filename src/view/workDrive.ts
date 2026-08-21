/**
 * The three work drive commands, which the palette and the panel's buttons both go through.
 *
 * Each one reads the drive afresh rather than trusting what the panel last showed: a developer
 * who mounted the drive in a terminal a second ago is exactly the case where a remembered answer
 * would be wrong. What is refused, and why, is `refusalOf`'s call; this only carries the words to
 * a message box, with the way out of the commonest refusal — nothing set — attached to it.
 */

import * as vscode from 'vscode';
import { SETTING } from '../mods/machine';
import {
  type Link,
  type WorkDrive,
  type WorkDriveAction,
  linksToMake,
  refusalOf,
} from '../mods/workDrive';
import { readMachineSettings } from '../platform/machine';
import {
  makeLinks,
  mount,
  platformRefusal,
  readLinks,
  readWorkDrive,
  unmount,
} from '../platform/workDrive';
import { findMods, prefixesOf } from '../platform/workspace';

/** The command ids, which are also what the panel's buttons ask for, by action. */
export const WORK_DRIVE_COMMAND: Readonly<Record<WorkDriveAction, string>> = {
  mount: 'enfusion.workDrive.mount',
  unmount: 'enfusion.workDrive.unmount',
  link: 'enfusion.workDrive.link',
};

/** Registers all three, and calls back after each so the panel shows what changed. */
export function registerWorkDriveCommands(
  log: vscode.LogOutputChannel,
  changed: () => void,
): vscode.Disposable {
  const commands = new WorkDriveCommands(log, changed);

  return vscode.Disposable.from(
    vscode.commands.registerCommand(WORK_DRIVE_COMMAND.mount, () => commands.mount()),
    vscode.commands.registerCommand(WORK_DRIVE_COMMAND.unmount, () => commands.unmount()),
    vscode.commands.registerCommand(WORK_DRIVE_COMMAND.link, () => commands.link()),
  );
}

class WorkDriveCommands {
  constructor(
    private readonly log: vscode.LogOutputChannel,
    private readonly changed: () => void,
  ) {}

  async mount(): Promise<void> {
    const drive = await this.current('mount');
    if (drive === undefined) {
      return;
    }

    await this.run(`mount ${drive.letter} from ${drive.source}`, () => mount(drive));
  }

  async unmount(): Promise<void> {
    const drive = await this.current('unmount');
    if (drive === undefined) {
      return;
    }

    await this.run(`unmount ${drive.letter}`, () => unmount(drive));
  }

  async link(): Promise<void> {
    const drive = await this.current('link');
    if (drive === undefined) {
      return;
    }

    const links = await readLinks(drive, prefixesOf(await findMods()));
    const making = linksToMake(links);

    const done = await this.run(`link ${making.length} mod(s) onto ${drive.letter}`, () =>
      makeLinks(making),
    );

    if (done) {
      await vscode.window.showInformationMessage(summarise(drive.letter, links, making));
    }
  }

  /** The drive as it is right now, or nothing at all when the action would only fail. */
  private async current(action: WorkDriveAction): Promise<WorkDrive | undefined> {
    const drive = await readWorkDrive(await readMachineSettings());
    const refusal = platformRefusal() ?? refusalOf(drive, action);

    if (refusal !== undefined) {
      this.log.warn(`${action}: ${refusal}`);
      await refuse(refusal, drive);
      return undefined;
    }

    return drive;
  }

  /**
   * Everything here is somebody's disk answering, so the failure worth showing is the one the
   * developer asked for: `subst` refusing a letter, a junction that could not be made.
   */
  private async run(what: string, work: () => Promise<void>): Promise<boolean> {
    try {
      await work();
      this.log.info(what);
      return true;
    } catch (error: unknown) {
      const message = error instanceof Error ? error.message : String(error);
      this.log.error(`could not ${what}: ${message}`);
      await vscode.window.showErrorMessage(`Could not ${what}: ${message}`);
      return false;
    } finally {
      this.changed();
    }
  }
}

/**
 * What a run of the link command did, and what it deliberately did not touch — one clause per
 * thing that happened, so that nothing said contradicts anything else said.
 */
function summarise(letter: string, links: readonly Link[], made: readonly Link[]): string {
  const occupied = links.filter((link) => link.state === 'occupied');
  const already = links.length - made.length - occupied.length;
  const said: string[] = [];

  if (made.length > 0) {
    said.push(`Linked ${made.length} mod(s) onto ${letter}.`);
  }
  if (already > 0) {
    said.push(`${already} already ${already === 1 ? 'was' : 'were'}.`);
  }
  if (occupied.length > 0) {
    const paths = occupied.map((link) => link.path).join(', ');
    said.push(`Left alone, because what is there is not a link: ${paths}.`);
  }

  return said.length === 0
    ? `No mod of this workspace has a prefix root to put onto ${letter}.`
    : said.join(' ');
}

/**
 * A refusal, with the one click that settles it where there is one. Which is only where a setting
 * is the thing at fault: offering the folder setting for "already mounted" would send a developer
 * to change something that is not wrong.
 */
async function refuse(message: string, drive: WorkDrive): Promise<void> {
  const fixable = drive.state === 'unset' || drive.state === 'elsewhere';
  if (!fixable) {
    await vscode.window.showWarningMessage(message);
    return;
  }

  const settings = 'Open Settings';
  if ((await vscode.window.showWarningMessage(message, settings)) === settings) {
    await vscode.commands.executeCommand('workbench.action.openSettings', SETTING.workDrive);
  }
}
