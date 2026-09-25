/**
 * Running a launch plan: the run folder made ready, and the game started in it.
 *
 * Every decision behind it was made in `src/mods/launch.ts` — which links belong there, which are
 * stale, what the command line is. This reads the disk, makes the links, spawns the process and
 * kills it again, and decides none of it.
 *
 * The game is started detached and its handle kept, so Stop or disposal can take the whole launch
 * down through `taskkill /T`. The process that is started is not always the process that ends up
 * running, and killing the tree is the only way to be sure the game is gone.
 */

import { type ChildProcess, execFile, spawn } from 'node:child_process';
import { copyFile, cp, mkdir, readFile, readdir, stat, writeFile } from 'node:fs/promises';
import { tmpdir, userInfo } from 'node:os';
import { promisify } from 'node:util';
import { borderlessSettingsOf } from '../mods/gameWindow';
import type {
  GameEntry,
  GameProgramFacts,
  GameRoot,
  LaunchPlan,
  LaunchProcess,
} from '../mods/launch';
import type { LaunchGame, ProcessExit } from '../mods/launchSession';
import { gameProgramOf } from '../mods/machine';
import type { GameBuild, GameSide, MachineSettings } from '../mods/machine';
import { windowsFolder, windowsName, windowsPath } from '../mods/paths';
import type { LinkFact } from '../mods/workDrive';
import { guardGameWindow } from './windowGuard';
import { linkFactAt, makeJunction, removeLink } from './workDrive';

const run = promisify(execFile);

/**
 * Where the run folder goes when no setting names a place: the per-user folder Windows keeps for
 * exactly this, with the temporary folder standing in on a machine that has no such thing.
 */
export function localAppData(): string {
  return process.env.LOCALAPPDATA ?? tmpdir();
}

/**
 * The Windows account the game runs as, by the name the game files its display settings under:
 * the one `GetUserName` answers, which is what `userInfo` asks too.
 */
export function windowsUser(): string {
  try {
    return userInfo().username;
  } catch {
    return process.env.USERNAME ?? '';
  }
}

/**
 * The game as a launch needs to know it: where it is, what starts it, and what its root holds.
 *
 * Both sides are read whichever the target puts up, because reading a path is two `stat` calls and
 * the alternative is threading the roles this far down. Which of them a refusal is allowed to
 * speak about is decided in the plan, where the roles are known.
 */
export async function readGameRoot(
  settings: MachineSettings,
  build: GameBuild,
  experimental = false,
): Promise<GameRoot> {
  const path = experimental ? settings.dayzExperimental : settings.dayz;
  const [client, server] = await Promise.all([
    programOf(settings, build, 'client', experimental),
    programOf(settings, build, 'server', experimental),
  ]);

  return {
    path,
    programs: { client, server },
    entries: await entriesOf(path),
  };
}

async function programOf(
  settings: MachineSettings,
  build: GameBuild,
  side: GameSide,
  experimental: boolean,
): Promise<GameProgramFacts> {
  const program = gameProgramOf(settings, build, side, experimental);

  return {
    ...program,
    present: program.path !== '' && (await exists(program.path)),
  };
}

/**
 * What is in a folder now and what each of them is — a link of ours, a link elsewhere, or something
 * real. Asked of the file patching root, whose links are the whole of what a launch remakes.
 * Nothing there is nothing to say, which is what a first launch finds.
 */
export async function readLinkFacts(folder: string): Promise<Map<string, LinkFact>> {
  const names = (await entriesOf(folder)).map((entry) => entry.name);
  const facts = await Promise.all(
    names.map(async (name) => [name, await linkFactAt(windowsPath(folder, name))] as const),
  );

  return new Map(facts);
}

/**
 * Which of the paths the plan asked about are there. Every one of them is a yes-or-no the plan
 * turns into a refusal or a command line, and none of them is judged here.
 */
export async function readFound(paths: readonly string[]): Promise<string[]> {
  const answers = await Promise.all(
    paths.map(async (path) => ({ path, there: await exists(path) })),
  );

  return answers.filter((answer) => answer.there).map((answer) => answer.path);
}

/**
 * The run folder made ready: the folders, then the links that are in the way taken off, then the
 * links made, then the files carried over, then the profile and the mission laid down, then each
 * client's window settings put right. In that order, because a link cannot be made where one
 * already is, and a layer cannot be copied into a folder that has not been made yet.
 *
 * What comes back is what could not be done and did not stop the launch: a client whose settings
 * could not be written still starts, in whatever window it was last left in.
 */
export async function prepareLaunch(plan: LaunchPlan): Promise<string[]> {
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
    await carry(copy.from, copy.to);
  }

  for (const copy of plan.copies) {
    await layer(copy.from, copy.to);
  }

  return settleWindowSettings(plan.windowSettings);
}

/**
 * Each of these settings files made to ask for the borderless window, and written only where that
 * changes it. The file is the game's own, in its own encoding, so it is read and written byte for
 * byte; one that is not there is made, because without it the game is not windowed at all.
 */
export async function settleWindowSettings(paths: readonly string[]): Promise<string[]> {
  const said: string[] = [];

  for (const path of paths) {
    try {
      const before = await readSettings(path);
      const after = borderlessSettingsOf(before ?? '');
      if (after !== before) {
        await mkdir(windowsFolder(path), { recursive: true });
        await writeFile(path, after, 'latin1');
      }
    } catch (error) {
      said.push(
        `The window settings in ${path} could not be written (${messageOf(error)}), so that ` +
          'client comes up in whatever window it was last left in.',
      );
    }
  }

  return said;
}

async function readSettings(path: string): Promise<string | undefined> {
  try {
    return await readFile(path, 'latin1');
  } catch (error) {
    if ((error as NodeJS.ErrnoException).code === 'ENOENT') {
      return undefined;
    }

    throw error;
  }
}

function messageOf(error: unknown): string {
  return error instanceof Error ? error.message : String(error);
}

/**
 * One file of the game root carried into the run folder — and left alone where a game already
 * running has it locked and a copy of it is already there.
 *
 * Windows refuses to overwrite a file another process holds open, and a launch started while the
 * last one is still up is exactly when that happens: what is being copied is the same bytes as the
 * copy that is already there, so refusing to launch over it would be refusing over nothing. A
 * missing destination is a different matter and is left to fail.
 */
async function carry(from: string, to: string): Promise<void> {
  try {
    await copyFile(from, to);
  } catch (error) {
    if (!(await exists(to))) {
      throw error;
    }
  }
}

/**
 * One layer of a profile or of a mission, laid over what is there already. A layer no mod keeps is
 * the ordinary case rather than a failure — the plan names every layer there could be — so a source
 * that is not there is passed over, and anything else is left to be reported as what it is.
 */
async function layer(from: string, to: string): Promise<void> {
  if (!(await exists(from))) {
    return;
  }

  await cp(from, to, { recursive: true, force: true });
}

/** A spawned game, with the platform process id retained for integrations that need it. */
export interface GameProcess extends LaunchGame {
  readonly pid: number | undefined;
}

/**
 * Starts the game in the folder the plan named. Detached and with its output ignored: the game
 * writes what it has to say to its `.RPT` in the profile, and a pipe nobody reads is a pipe that
 * fills up and stops the process it belongs to. The returned promise confirms the OS accepted the
 * spawn; an earlier `error` rejects it, while every later way for the process to disappear is an
 * ordinary `ProcessExit`.
 *
 * A client is started with a guard beside it for the first seconds of its window, put up before it
 * and put down with it — see `src/platform/windowGuard.ts`. What the guard did is said to `said`.
 */
export async function startGame(
  process_: LaunchProcess,
  prefix: readonly string[] = [],
  said: (line: string) => void = () => undefined,
): Promise<GameProcess> {
  // A prefix is another program that starts ours: Sandboxie's `Start.exe`, which is what gives the
  // second client a Steam of its own. It goes in front whole, so the thing that is actually spawned
  // is that program and the game is its argument — and it is told to wait, so that the process
  // handle kept here is one that lives as long as the game does. See `src/mods/sandbox.ts`.
  const [program = process_.program, ...before] = prefix;
  const argued = prefix.length === 0 ? [] : [...before, process_.program];

  // The guard knows the game by its program and its start, not by this process: in a box, the game
  // is a child of Sandboxie's service rather than of the `Start.exe` spawned here.
  const guard =
    process_.role === 'server' ? undefined : guardGameWindow(windowsName(process_.program), said);

  const child: ChildProcess = spawn(program, [...argued, ...process_.arguments], {
    cwd: process_.cwd,
    detached: true,
    stdio: 'ignore',
    windowsHide: false,
  });

  let spawned = false;
  const exited = new Promise<ProcessExit>((resolve) => {
    child.once('exit', (code, signal) => {
      resolve(processExitOf(code, signal));
    });
    // A post-spawn process error need not be followed by `exit`, but still ends this handle's wait.
    child.on('error', () => {
      if (spawned) {
        resolve({ kind: 'unknown' });
      }
    });
  });

  try {
    await new Promise<void>((resolve, reject) => {
      const onSpawn = (): void => {
        spawned = true;
        child.off('error', onError);
        resolve();
      };
      const onError = (error: Error): void => {
        child.off('spawn', onSpawn);
        reject(error);
      };

      child.once('spawn', onSpawn);
      child.once('error', onError);
    });
  } catch (error) {
    guard?.stop();
    throw error;
  }
  child.unref();
  void exited.then(() => guard?.stop());

  return {
    role: process_.role,
    pid: child.pid,
    exited,
    kill: async () => {
      guard?.stop();
      await kill(child);
    },
  };
}

function processExitOf(code: number | null, signal: NodeJS.Signals | null): ProcessExit {
  if (code !== null) {
    return { kind: 'code', code };
  }

  if (signal !== null) {
    return { kind: 'signal', signal };
  }

  return { kind: 'unknown' };
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
