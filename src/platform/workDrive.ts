/**
 * The work drive as the machine holds it: what `subst` says is mounted, what the disk says is at
 * each link, and the four operations that change either. What any of it means is decided in
 * `src/mods/workDrive.ts`; nothing here refuses anything or picks what to do.
 *
 * A link is an NTFS junction rather than a symlink, because a junction is the one kind of link
 * Windows makes without asking for Developer Mode or for an elevated editor — which is why
 * `mklink /J` is what every DayZ work drive has been set up with by hand.
 */

import { execFile } from 'node:child_process';
import type { Stats } from 'node:fs';
import { lstat, readlink, rmdir, symlink, unlink } from 'node:fs/promises';
import { promisify } from 'node:util';
import type { MachineSettings } from '../mods/machine';
import { mountArguments, mountedAt, unmountArguments } from '../mods/subst';
import {
  type Link,
  type LinkFact,
  type Prefix,
  type WorkDrive,
  driveLetterOf,
  linkPathOf,
  linksOf,
  workDriveOf,
} from '../mods/workDrive';

const run = promisify(execFile);

/**
 * Why none of this can be done on this machine, or undefined where it can. A work drive is a
 * Windows drive letter and `subst` is a Windows program, so on anything else every button refuses
 * for the same reason — said here rather than in the domain, because which machine this is is a
 * fact about the machine and not about the drive.
 */
export function platformRefusal(): string | undefined {
  return process.platform === 'win32'
    ? undefined
    : 'A work drive is a Windows drive letter, and this machine is not Windows.';
}

/**
 * Where the letter points now, against where the settings say it should.
 *
 * With no folder set, the one the drive is already up from is taken as the setting: a work drive
 * that DayZ Tools or a `subst` in somebody's startup script put up is the work drive of this
 * machine, and asking a developer to type its path back in so that the extension will admit it
 * exists is asking for nothing. Only a machine with neither — no setting and no drive — has
 * nothing to mount, and that is the one case the buttons refuse over.
 */
export async function readWorkDrive(settings: MachineSettings): Promise<WorkDrive> {
  const letter = driveLetterOf(settings.workDriveLetter);
  const at = mountedAt(await mounts(), letter);

  return workDriveOf(letter, settings.workDrive === '' ? at : settings.workDrive, at);
}

/**
 * What is at each mod's place on the drive. With the drive down nothing on it is asked about: the
 * paths would all answer "not there", which reads as "not linked" and would be a lie.
 */
export async function readLinks(
  drive: WorkDrive,
  prefixes: readonly Prefix[],
): Promise<Link[]> {
  if (drive.at === '') {
    return linksOf(prefixes, drive, new Map());
  }

  const facts = await Promise.all(
    prefixes.map(async (prefix) => {
      const path = linkPathOf(drive.letter, prefix.name);
      return [path, await linkFactAt(path)] as const;
    }),
  );

  return linksOf(prefixes, drive, new Map(facts));
}

export async function mount(drive: WorkDrive): Promise<void> {
  await run('subst', mountArguments(drive.letter, drive.source));
}

export async function unmount(drive: WorkDrive): Promise<void> {
  await run('subst', unmountArguments(drive.letter));
}

/**
 * Makes the links the plan asks for, taking off whatever link is in the way of one first. One at a
 * time: a junction costs nothing to make, and a failure that names one mod is worth more than a
 * handful thrown at once.
 */
export async function makeLinks(links: readonly Link[]): Promise<void> {
  for (const link of links) {
    if (link.at !== '') {
      await removeLink(link.path);
    }

    await makeJunction(link.path, link.target);
  }
}

/**
 * One junction, at the path and onto the folder given. The one kind of link Windows makes without
 * Developer Mode, which is why the launch folder is built out of these as well.
 */
export async function makeJunction(path: string, target: string): Promise<void> {
  await symlink(target, path, 'junction');
}

/** What `subst` prints with no arguments: every letter it has put up, and the folder behind it. */
async function mounts(): Promise<string> {
  if (platformRefusal() !== undefined) {
    return '';
  }

  try {
    const { stdout } = await run('subst');
    return stdout;
  } catch {
    // Which is what it does with nothing mounted at all, and the answer to that is "nowhere".
    return '';
  }
}

export async function linkFactAt(path: string): Promise<LinkFact> {
  const stats = await statOf(path);
  if (stats === undefined) {
    return { kind: 'none' };
  }

  if (!stats.isSymbolicLink()) {
    return { kind: 'occupied' };
  }

  const target = await targetOf(path);

  // A link that will not say where it points is still a link in the way, and still not ours.
  return target === undefined ? { kind: 'occupied' } : { kind: 'link', target };
}

/** Of the link itself, never of what it points at: the question is what sits at the path. */
async function statOf(path: string): Promise<Stats | undefined> {
  try {
    return await lstat(path);
  } catch {
    return undefined;
  }
}

async function targetOf(path: string): Promise<string | undefined> {
  try {
    return await readlink(path);
  } catch {
    return undefined;
  }
}

/**
 * Takes the link off and leaves what it points at alone. `unlink` is what does that for a
 * junction; where it will not, `rmdir` removes the reparse point rather than what is behind it —
 * which is why neither the mod's sources nor anything else on the drive can be lost here.
 */
export async function removeLink(path: string): Promise<void> {
  try {
    await unlink(path);
  } catch {
    await rmdir(path);
  }
}
