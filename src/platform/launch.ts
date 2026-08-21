/**
 * Running a launch plan: the run folder made ready, and the game started in it.
 *
 * Every decision behind it was made in `src/mods/launch.ts` — which links belong there, which are
 * stale, what the command line is. This reads the disk, makes the links, spawns the process and
 * kills it again, and decides none of it.
 *
 * The game is started detached and its handle kept, so that two things hold at once: closing the
 * editor does not take the game down with it, and Stop does — through `taskkill /T`, because the
 * process that is started is not always the process that ends up running, and killing the tree is
 * the only way to be sure the game is gone.
 */

import { type ChildProcess, execFile, spawn } from 'node:child_process';
import { copyFile, mkdir, readdir, stat } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { promisify } from 'node:util';
import type { GameEntry, GameRoot, LaunchPlan, LaunchProcess } from '../mods/launch';
import { gameExecutableOf } from '../mods/machine';
import type { MachineSettings } from '../mods/machine';
import { windowsFolder, windowsPath } from '../mods/paths';
import type { LinkFact } from '../mods/workDrive';
import { linkFactAt, makeJunction, removeLink } from './workDrive';

const run = promisify(execFile);

/**
 * Where the run folder goes when no setting names a place: the per-user folder Windows keeps for
 * exactly this, with the temporary folder standing in on a machine that has no such thing.
 */
export function localAppData(): string {
  return process.env.LOCALAPPDATA ?? tmpdir();
}

/** The game as a launch needs to know it: where it is, what starts it, and what its root holds. */
export async function readGameRoot(settings: MachineSettings): Promise<GameRoot> {
  const executable = gameExecutableOf(settings);

  return {
    path: settings.dayz,
    executable,
    present: await exists(executable),
    entries: await entriesOf(settings.dayz),
  };
}

/** What is in the run folder now, by the name it goes by there. Nothing there is nothing to say. */
export async function readRunRoot(root: string): Promise<Map<string, LinkFact>> {
  const names = (await entriesOf(root)).map((entry) => entry.name);
  const facts = await Promise.all(
    names.map(async (name) => [name, await linkFactAt(windowsPath(root, name))] as const),
  );

  return new Map(facts);
}

/**
 * The run folder made ready: the folders, then the links that are in the way taken off, then the
 * links made, then the files carried over. In that order, because a link cannot be made where one
 * already is.
 */
export async function prepareLaunch(plan: LaunchPlan): Promise<void> {
  for (const folder of plan.folders) {
    await mkdir(folder, { recursive: true });
  }

  for (const path of plan.filePatching.remove) {
    await removeLink(path);
  }

  for (const junction of plan.filePatching.junctions) {
    await makeJunction(junction.path, junction.target);
  }

  for (const copy of plan.filePatching.copies) {
    await mkdir(windowsFolder(copy.to), { recursive: true });
    await copyFile(copy.from, copy.to);
  }
}

/** A game that is running: what it is, and the two things anybody wants from it. */
export interface GameProcess {
  readonly role: LaunchProcess['role'];
  readonly pid: number | undefined;
  /** Resolves when the game is gone, with whatever it exited with. */
  readonly exited: Promise<number | undefined>;
  /** Takes the game down, tree and all. Doing it twice is not an error. */
  kill(): Promise<void>;
}

/**
 * Starts the game in the folder the plan named. Detached and with its output ignored: the game
 * writes what it has to say to its `.RPT` in the profile, and a pipe nobody reads is a pipe that
 * fills up and stops the process it belongs to.
 */
export function startGame(process_: LaunchProcess): GameProcess {
  const child: ChildProcess = spawn(process_.program, [...process_.arguments], {
    cwd: process_.cwd,
    detached: true,
    stdio: 'ignore',
    windowsHide: false,
  });
  child.unref();

  const exited = new Promise<number | undefined>((resolve) => {
    child.on('exit', (code) => {
      resolve(code ?? undefined);
    });
    // A program that could not be started never exits, so the error is what ends the waiting.
    child.on('error', () => {
      resolve(undefined);
    });
  });

  return {
    role: process_.role,
    pid: child.pid,
    exited,
    kill: () => kill(child),
  };
}

/**
 * `taskkill /T` rather than `ChildProcess.kill`: the game spawns a crash reporter and a BattlEye
 * launcher of its own, and a signal to the one process we hold leaves those behind — which is the
 * whole reason Stop is worth having over the task manager.
 */
async function kill(child: ChildProcess): Promise<void> {
  const pid = child.pid;
  if (pid === undefined || child.exitCode !== null || child.signalCode !== null) {
    return;
  }

  try {
    await run('taskkill', ['/PID', String(pid), '/T', '/F']);
  } catch {
    // Which is what it answers for a process that has already gone; the kill below is the fallback
    // for a machine where `taskkill` is not the way.
    child.kill();
  }
}

/** A folder that is not there holds nothing, which is what a first launch finds. */
async function entriesOf(folder: string): Promise<GameEntry[]> {
  if (folder === '') {
    return [];
  }

  try {
    const entries = await readdir(folder, { withFileTypes: true });

    return entries.map((entry) => ({ name: entry.name, directory: entry.isDirectory() }));
  } catch {
    return [];
  }
}

async function exists(path: string): Promise<boolean> {
  try {
    await stat(path);
    return true;
  } catch {
    return false;
  }
}
