/**
 * The box, made and brought up: the commands run, the box's own folder read, and the wait for the
 * sign-in. What any of it means is decided in `src/mods/sandbox.ts`.
 *
 * The whole of it is one call. Everything a second client needs before it can be started — a box
 * that exists, a Steam running in it, an account signed in to that Steam — is either already true
 * or is made true here, and the developer presses one button rather than pressing it, reading a
 * sentence, doing something, and pressing it again. That is what this replaced: a button that
 * started a Steam and then asked to be pressed a second time, which is a step nobody who is
 * launching a game wants to be told about.
 */

import { execFile, spawn } from 'node:child_process';
import { readFile, realpath, stat } from 'node:fs/promises';
import { setTimeout as wait } from 'node:timers/promises';
import { promisify } from 'node:util';
import { windowsFolder, windowsName, windowsPath } from '../../mods/paths';
import {
  BOX_SETTINGS,
  SIGN_IN_PATIENCE,
  type Sandbox,
  boxExistsOf,
  boxPidsOf,
  boxPrefixOf,
  boxRootOf,
  connectionLogPathOf,
  imagePidsOf,
  inBoxOf,
  loginUsersPathOf,
  previousConnectionLogPathOf,
  steamAccountIdOf,
  steamConnectedSinceOf,
  steamCommandOf,
  steamConfigPathOf,
} from '../../mods/launch/sandbox';
import { type GameProcess, settleWindowSettings } from './launch';

const run = promisify(execFile);

/** Steam's own name, which is what the box is asked whether it is running. */
const STEAM_IMAGE = 'steam.exe';

/** A process of the final client, which the bootstrap updater does not put up. */
const STEAM_CLIENT_IMAGE = 'steamwebhelper.exe';

/** How often the wait looks again. */
const POLL = 2000;

/** How often the waiting says so, so that a wait for a human does not look like a hang. */
const SAY_EVERY = 15 * 1000;

/** The host operations behind one `openSandbox` call, replaceable by its deterministic test. */
export interface SandboxRuntime {
  now(): number;
  boxExists(sandbox: Sandbox, signal?: AbortSignal): Promise<boolean>;
  makeBox(sandbox: Sandbox, signal?: AbortSignal): Promise<string | undefined>;
  steamFiles(sandbox: Sandbox, signal?: AbortSignal): Promise<SandboxSteamFiles | undefined>;
  imageIsUp(sandbox: Sandbox, image: string, signal?: AbortSignal): Promise<boolean>;
  imageStartedAt(
    sandbox: Sandbox,
    image: string,
    signal?: AbortSignal,
  ): Promise<number | undefined>;
  startSteam(sandbox: Sandbox): void;
  accountId(
    files: readonly string[],
    account: string,
    signal?: AbortSignal,
  ): Promise<string | undefined>;
  readConnectionLogs(logs: readonly string[], signal?: AbortSignal): Promise<string>;
  sleep(milliseconds: number, signal?: AbortSignal): Promise<void>;
}

/** The boxed Steam files needed to distinguish current identity from live readiness. */
export interface SandboxSteamFiles {
  readonly identityFiles: readonly string[];
  /** The rotated predecessor first, then the live connection log. */
  readonly connectionLogs: readonly string[];
}

const SYSTEM_RUNTIME: SandboxRuntime = {
  now: Date.now,
  boxExists,
  makeBox,
  steamFiles,
  imageIsUp,
  imageStartedAt,
  startSteam,
  accountId: readSteamAccountId,
  readConnectionLogs,
  sleep,
};

/**
 * The box, ready for a game to be started in it — or the sentence saying why it is not.
 *
 * In the order the answers are cheapest: a box that is not there is made, a Steam that is not up
 * is started, and a sign-in that has not happened is waited for. Every one of those is skipped
 * where it is already done, so the ordinary second press of an evening does none of them and
 * starts the client at once. An aborted signal interrupts the wait and prevents the next step.
 */
export async function openSandbox(
  sandbox: Sandbox,
  say: (text: string) => void,
  signal?: AbortSignal,
  runtime: SandboxRuntime = SYSTEM_RUNTIME,
): Promise<string | undefined> {
  signal?.throwIfAborted();

  if (!(await runtime.boxExists(sandbox, signal))) {
    signal?.throwIfAborted();
    say(`Making the ${sandbox.box} sandbox.`);

    const failed = await runtime.makeBox(sandbox, signal);
    signal?.throwIfAborted();
    if (failed !== undefined) {
      return failed;
    }
  }

  signal?.throwIfAborted();
  const files = await runtime.steamFiles(sandbox, signal);
  signal?.throwIfAborted();
  if (files === undefined) {
    return `Steam is at ${sandbox.steam}, which is not a path this can find inside the ${sandbox.box} sandbox.`;
  }

  const steamIsRunning = await runtime.imageIsUp(sandbox, STEAM_IMAGE, signal);
  signal?.throwIfAborted();

  if (steamIsRunning) {
    // A Steam that is up but not connected yet is either bootstrapping or being signed in. Both are
    // worth waiting for rather than starting a second one on top of it. Its process start time is
    // the boundary that stops an unclosed success from an older Steam answering for this one.
    const ready = await steamReady(sandbox, files, runtime, signal);
    signal?.throwIfAborted();

    return ready ? undefined : await waitForSteam(sandbox, files, say, signal, runtime);
  }

  say(`Starting Steam in the ${sandbox.box} sandbox as ${sandbox.account}.`);
  signal?.throwIfAborted();
  runtime.startSteam(sandbox);

  return waitForSteam(sandbox, files, say, signal, runtime);
}

/**
 * The game in the box, with Stop taught how to reach it.
 *
 * `Start.exe` does not start the program itself — it asks Sandboxie's service to, and the service
 * is what the sandboxed process is a child of. So the tree that `taskkill /T` walks from the
 * process this extension holds ends at `Start.exe`, and Stop takes down the thing that was waiting
 * for the game rather than the game: pressed, it says the launch is over, and the second client
 * keeps playing with nothing in the editor to show for it. It was measured that way rather than
 * reasoned about — a boxed process killed by its parent's tree survives it.
 *
 * What does reach it is its own pid, and finding that is the same two lists as finding Steam:
 * what is in the box, and where every process of that program is. Which is also what keeps this
 * off the first client — that one is the same executable, and it is not in the box.
 */
export function sandboxedGame(game: GameProcess, sandbox: Sandbox, image: string): GameProcess {
  return {
    ...game,
    kill: async () => {
      await game.kill();
      await killInBox(sandbox, image);
    },
  };
}

async function killInBox(sandbox: Sandbox, image: string): Promise<void> {
  const [inside, running] = await Promise.all([boxPids(sandbox), imagePids(image)]);

  for (const pid of running.filter((found) => inside.includes(found))) {
    try {
      await run('taskkill', ['/PID', String(pid), '/T', '/F']);
    } catch {
      // Which is what it answers for a process that has gone in the meantime, and that is the
      // outcome this was after.
    }
  }
}

/**
 * The wait itself: every couple of seconds, has the final Steam client completed its logon.
 *
 * Giving up is not a failure of the launch so much as the end of what waiting can do — the box is
 * up and the sign-in is a person's to finish — so what comes back says exactly that, and pressing
 * the button again is the whole of the recovery.
 */
async function waitForSteam(
  sandbox: Sandbox,
  files: SandboxSteamFiles,
  say: (text: string) => void,
  signal?: AbortSignal,
  runtime: SandboxRuntime = SYSTEM_RUNTIME,
): Promise<string | undefined> {
  signal?.throwIfAborted();
  const until = runtime.now() + SIGN_IN_PATIENCE;
  let said = runtime.now();

  while (runtime.now() < until) {
    await runtime.sleep(POLL, signal);
    signal?.throwIfAborted();

    const ready = await steamReady(sandbox, files, runtime, signal);
    signal?.throwIfAborted();

    if (ready) {
      say(`Steam is ready in the ${sandbox.box} sandbox as ${sandbox.account}.`);
      signal?.throwIfAborted();
      return undefined;
    }

    if (runtime.now() - said >= SAY_EVERY) {
      said = runtime.now();
      say(`Waiting for ${sandbox.account} to sign in to the ${sandbox.box} sandbox.`);
    }
  }

  signal?.throwIfAborted();
  return (
    `Steam has not finished starting and signing in as ${sandbox.account} in the ${sandbox.box} ` +
    'sandbox. Finish the sign-in there, then press the second-client button again.'
  );
}

/**
 * The four independent facts that make the boxed Steam usable by a game.
 *
 * `steam.exe` alone is also the bootstrap updater. `steamwebhelper.exe` appears only with the
 * final client, but before logon completes. Steam's account files map the requested name to an
 * account id, while the connection log says that same account is live. None is sufficient alone.
 */
async function steamReady(
  sandbox: Sandbox,
  files: SandboxSteamFiles,
  runtime: SandboxRuntime,
  signal?: AbortSignal,
): Promise<boolean> {
  const [client, accountId] = await Promise.all([
    runtime.imageIsUp(sandbox, STEAM_CLIENT_IMAGE, signal),
    runtime.accountId(files.identityFiles, sandbox.account, signal),
  ]);
  signal?.throwIfAborted();

  if (!client || accountId === undefined) {
    return false;
  }

  // The logs can be several megabytes after rotation. Do not reread them while the bootstrap
  // updater or the login window already proves that the final client is not ready.
  const connection = await runtime.readConnectionLogs(files.connectionLogs, signal);
  signal?.throwIfAborted();

  // Read this last. If Steam restarted while the other facts were sampled, the newer process time
  // wins and the preceding process's connection cannot be mixed into a ready snapshot.
  const steamStartedAt = await runtime.imageStartedAt(sandbox, STEAM_IMAGE, signal);
  signal?.throwIfAborted();

  return (
    steamStartedAt !== undefined &&
    steamConnectedSinceOf(connection, steamStartedAt, accountId)
  );
}

/** Whether Sandboxie's configuration holds the box: `SbieIni` answers a box that is not there with nothing. */
async function boxExists(sandbox: Sandbox, signal?: AbortSignal): Promise<boolean> {
  try {
    const { stdout } = await run(sandbox.ini, ['query', sandbox.box, 'Enabled'], { signal });

    return boxExistsOf(stdout);
  } catch {
    // Which is also what a Sandboxie that cannot be run answers, and the making below says so.
    return false;
  }
}

/**
 * Makes the box, one setting at a time and in order: the first is what puts the section into
 * Sandboxie's configuration, and Sandboxie fills the rest of a new box in with its own defaults.
 * One at a time because they all write the one file.
 */
async function makeBox(sandbox: Sandbox, signal?: AbortSignal): Promise<string | undefined> {
  for (const [setting, value] of BOX_SETTINGS) {
    signal?.throwIfAborted();

    try {
      await run(sandbox.ini, ['set', sandbox.box, setting, value], { signal });
    } catch (error) {
      return (
        `The ${sandbox.box} sandbox could not be made: ${sandbox.ini} set ${sandbox.box} ` +
        `${setting} ${value} failed with ${error instanceof Error ? error.message : String(error)}.`
      );
    }
  }

  return undefined;
}

/**
 * Whether a Steam is running inside the box.
 *
 * Two questions rather than one, because neither answers it alone: Sandboxie says which processes
 * are in the box but not what they are, and `tasklist` says where every Steam is but not which of
 * them is sandboxed. Where the two lists meet is a Steam in this box.
 */
async function imageIsUp(
  sandbox: Sandbox,
  image: string,
  signal?: AbortSignal,
): Promise<boolean> {
  return (await boxedImagePids(sandbox, image, signal)).length !== 0;
}

/** When the newest boxed process with this image was started, in epoch milliseconds. */
async function imageStartedAt(
  sandbox: Sandbox,
  image: string,
  signal?: AbortSignal,
): Promise<number | undefined> {
  const pids = await boxedImagePids(sandbox, image, signal);
  if (pids.length === 0) {
    return undefined;
  }

  const script = [
    '& { param([string]$PidList)',
    "$ids = [int[]]$PidList.Split(':')",
    '$found = Get-Process -Id $ids -ErrorAction SilentlyContinue',
    '$latest = $found | Sort-Object -Property StartTime -Descending | Select-Object -First 1',
    "if ($null -ne $latest) { $latest.StartTime.ToUniversalTime().ToString('O') }",
    '}',
  ].join('; ');

  try {
    const { stdout } = await run(
      'powershell.exe',
      ['-NoProfile', '-NonInteractive', '-Command', script, pids.join(':')],
      { signal, windowsHide: true },
    );
    const startedAt = Date.parse(stdout.trim());

    return Number.isNaN(startedAt) ? undefined : startedAt;
  } catch {
    return undefined;
  }
}

async function boxedImagePids(
  sandbox: Sandbox,
  image: string,
  signal?: AbortSignal,
): Promise<number[]> {
  const [inside, running] = await Promise.all([
    boxPids(sandbox, signal),
    imagePids(image, signal),
  ]);

  return running.filter((pid) => inside.includes(pid));
}

async function boxPids(sandbox: Sandbox, signal?: AbortSignal): Promise<number[]> {
  try {
    const { stdout } = await run(sandbox.start, [`/box:${sandbox.box}`, '/listpids'], { signal });

    return boxPidsOf(stdout);
  } catch {
    return [];
  }
}

async function imagePids(image: string, signal?: AbortSignal): Promise<number[]> {
  try {
    const { stdout } = await run(
      'tasklist',
      ['/FI', `IMAGENAME eq ${image}`, '/NH', '/FO', 'CSV'],
      { signal, windowsHide: true },
    );

    return imagePidsOf(stdout);
  } catch {
    return [];
  }
}

/**
 * Steam, started inside the box and left alone: it is meant to outlive this launch and the next
 * one, since signing in is the expensive part and a Steam that stays up is a Steam nobody signs in
 * to twice. A second run of it while it is up is Steam's own business to make cheap, and it does:
 * it hands off to the instance that is running and exits.
 */
function startSteam(sandbox: Sandbox): void {
  const [program = '', ...rest] = [...boxPrefixOf(sandbox), ...steamCommandOf(sandbox)];
  const child = spawn(program, rest, { detached: true, stdio: 'ignore', windowsHide: false });
  child.on('error', () => {
    // Nothing to do about it here: what it costs is a sign-in that never happens, and the wait
    // above is what says so — in more words than an error out of a spawn would have.
  });
  child.unref();
}

/**
 * The box's own copies of these window settings, put right the way the files on the disk are.
 *
 * A game in the box reads its settings through the box: the file on the disk until the game first
 * writes them, and the box's copy ever after. Only a copy that is there is touched — where there is
 * none, the file on the disk is the one read, and it has been put right already.
 */
export async function settleBoxedWindowSettings(
  sandbox: Sandbox,
  paths: readonly string[],
): Promise<string[]> {
  const root = await boxRoot(sandbox);
  const copies: string[] = [];

  for (const path of paths) {
    try {
      // Through a `subst` drive to the folder it is mounted from, which is what the box files under.
      // The promise `realpath` asks the operating system and resolves the mount; the JavaScript
      // `realpathSync`, which walks the path itself, would hand the drive letter back.
      const real = windowsPath(await realpath(windowsFolder(path)), windowsName(path));
      const copy = inBoxOf(root, real);
      if (copy !== undefined && (await isFile(copy))) {
        copies.push(copy);
      }
    } catch {
      // No folder, no copy: the game has not written its settings anywhere yet.
    }
  }

  return settleWindowSettings(copies);
}

async function isFile(path: string): Promise<boolean> {
  try {
    return (await stat(path)).isFile();
  } catch {
    return false;
  }
}

/** The folder the box keeps its files in. */
async function boxRoot(sandbox: Sandbox, signal?: AbortSignal): Promise<string> {
  return boxRootOf(
    await fileRootPath(sandbox, signal),
    sandbox.box,
    process.env.USERNAME ?? '',
    process.env.SystemDrive ?? 'C:',
  );
}

/** The boxed copies of Steam's current identity and live connection state. */
async function steamFiles(
  sandbox: Sandbox,
  signal?: AbortSignal,
): Promise<SandboxSteamFiles | undefined> {
  const root = await boxRoot(sandbox, signal);
  const loginUsers = loginUsersPathOf(root, sandbox.steam);
  const config = steamConfigPathOf(root, sandbox.steam);
  const connectionLog = connectionLogPathOf(root, sandbox.steam);
  const previousConnectionLog = previousConnectionLogPathOf(root, sandbox.steam);

  if (loginUsers === undefined || config === undefined || connectionLog === undefined || previousConnectionLog === undefined) {
    return undefined;
  }

  return { identityFiles: [loginUsers, config], connectionLogs: [previousConnectionLog, connectionLog] };
}

/** Where Sandboxie puts its boxes, which is nothing at all on an installation nobody has moved. */
async function fileRootPath(sandbox: Sandbox, signal?: AbortSignal): Promise<string> {
  try {
    const { stdout } = await run(
      sandbox.ini,
      ['query', 'GlobalSettings', 'FileRootPath'],
      { signal },
    );

    return stdout;
  } catch {
    return '';
  }
}

/**
 * The id Steam assigned to the account this box is meant to use.
 *
 * This establishes identity only. `steamReady` also requires that exact id in the successful live
 * connection state, so another remembered account cannot answer for the requested one.
 */
export async function readSteamAccountId(
  files: readonly string[],
  account: string,
  signal?: AbortSignal,
): Promise<string | undefined> {
  for (const file of files) {
    signal?.throwIfAborted();
    try {
      const id = steamAccountIdOf(await readFile(file, { encoding: 'utf8', signal }), account);
      if (id !== undefined) {
        return id;
      }
    } catch {
      signal?.throwIfAborted();
      // A successful first sign-in need not write loginusers.vdf. Try the boxed config too.
    }
  }

  return undefined;
}

/** The rotated predecessor and live log, in chronological order. Either may be absent. */
async function readConnectionLogs(
  logs: readonly string[],
  signal?: AbortSignal,
): Promise<string> {
  const parts = await Promise.all(
    logs.map(async (log) => {
      try {
        return await readFile(log, { encoding: 'utf8', signal });
      } catch {
        signal?.throwIfAborted();
        return '';
      }
    }),
  );

  return parts.join('\n');
}

function sleep(milliseconds: number, signal?: AbortSignal): Promise<void> {
  signal?.throwIfAborted();

  return wait(milliseconds, undefined, { signal });
}
