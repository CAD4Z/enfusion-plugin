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
import { readFile, stat } from 'node:fs/promises';
import { promisify } from 'node:util';
import {
  BOX_SETTINGS,
  type Sandbox,
  boxExistsOf,
  boxPidsOf,
  boxPrefixOf,
  boxRootOf,
  imagePidsOf,
  loginUsersPathOf,
  signedInNowOf,
  signedInOf,
  steamCommandOf,
} from '../mods/sandbox';
import type { GameProcess } from './launch';

const run = promisify(execFile);

/** Steam's own name, which is what the box is asked whether it is running. */
const STEAM_IMAGE = 'steam.exe';

/** How often the wait looks again. */
const POLL = 2000;

/**
 * How long it waits for the sign-in before handing the console back.
 *
 * Long, because the first sign-in into a fresh box is a password and a Steam Guard code out of an
 * email, and a developer typing those is doing exactly what this is waiting for. Not endless,
 * because the button is held by the wait: giving up says what is missing and leaves the box up, so
 * the press after it finds a Steam that is already there and goes straight to the game.
 */
const PATIENCE = 3 * 60 * 1000;

/** How often the waiting says so, so that a wait for a human does not look like a hang. */
const SAY_EVERY = 15 * 1000;

/**
 * The box, ready for a game to be started in it — or the sentence saying why it is not.
 *
 * In the order the answers are cheapest: a box that is not there is made, a Steam that is not up
 * is started, and a sign-in that has not happened is waited for. Every one of those is skipped
 * where it is already done, so the ordinary second press of an evening does none of them and
 * starts the client at once.
 */
export async function openSandbox(
  sandbox: Sandbox,
  say: (text: string) => void,
): Promise<string | undefined> {
  if (!(await boxExists(sandbox))) {
    say(`Making the ${sandbox.box} sandbox.`);

    const failed = await makeBox(sandbox);
    if (failed !== undefined) {
      return failed;
    }
  }

  const users = await loginUsersPath(sandbox);
  if (users === undefined) {
    return `Steam is at ${sandbox.steam}, which is not a path this can find inside the ${sandbox.box} sandbox.`;
  }

  if (await steamIsUp(sandbox)) {
    // A Steam that is up but has never signed this account in is a Steam somebody is in the middle
    // of signing in to, which is worth waiting for rather than starting a second one on top of.
    // Nothing was started here, so there is no moment for the record to be newer than: a zero.
    return signedInNowOf(await signInWritten(users, sandbox.account), 0, Date.now())
      ? undefined
      : await waitForSignIn(sandbox, users, 0, say);
  }

  // Read before the start rather than after it, so that a file Steam rewrites the moment it signs
  // in cannot be mistaken for the one it left there last time.
  const since = Date.now();
  say(`Starting Steam in the ${sandbox.box} sandbox as ${sandbox.account}.`);
  startSteam(sandbox);

  return waitForSignIn(sandbox, users, since, say);
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
 * The wait itself: every couple of seconds, has Steam written down that the account is signed in.
 *
 * Giving up is not a failure of the launch so much as the end of what waiting can do — the box is
 * up and the sign-in is a person's to finish — so what comes back says exactly that, and pressing
 * the button again is the whole of the recovery.
 */
async function waitForSignIn(
  sandbox: Sandbox,
  users: string,
  since: number,
  say: (text: string) => void,
): Promise<string | undefined> {
  const until = Date.now() + PATIENCE;
  let said = Date.now();

  while (Date.now() < until) {
    await sleep(POLL);

    if (signedInNowOf(await signInWritten(users, sandbox.account), since, Date.now())) {
      say(`${sandbox.account} is signed in to the ${sandbox.box} sandbox.`);
      return undefined;
    }

    if (Date.now() - said >= SAY_EVERY) {
      said = Date.now();
      say(`Waiting for ${sandbox.account} to sign in to the ${sandbox.box} sandbox.`);
    }
  }

  return (
    `${sandbox.account} has not signed in to the ${sandbox.box} sandbox. Sign in to the Steam ` +
    'that is up in it, then press the second-client button again.'
  );
}

/** Whether Sandboxie's configuration holds the box: `SbieIni` answers a box that is not there with nothing. */
async function boxExists(sandbox: Sandbox): Promise<boolean> {
  try {
    const { stdout } = await run(sandbox.ini, ['query', sandbox.box, 'Enabled']);

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
async function makeBox(sandbox: Sandbox): Promise<string | undefined> {
  for (const [setting, value] of BOX_SETTINGS) {
    try {
      await run(sandbox.ini, ['set', sandbox.box, setting, value]);
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
async function steamIsUp(sandbox: Sandbox): Promise<boolean> {
  const [inside, steams] = await Promise.all([boxPids(sandbox), imagePids(STEAM_IMAGE)]);

  return steams.some((pid) => inside.includes(pid));
}

async function boxPids(sandbox: Sandbox): Promise<number[]> {
  try {
    const { stdout } = await run(sandbox.start, [`/box:${sandbox.box}`, '/listpids']);

    return boxPidsOf(stdout);
  } catch {
    return [];
  }
}

async function imagePids(image: string): Promise<number[]> {
  try {
    const { stdout } = await run('tasklist', ['/FI', `IMAGENAME eq ${image}`, '/NH', '/FO', 'CSV']);

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

/** Where the box keeps its own copy of Steam's record of who is signed in. */
async function loginUsersPath(sandbox: Sandbox): Promise<string | undefined> {
  const root = boxRootOf(
    await fileRootPath(sandbox),
    sandbox.box,
    process.env.USERNAME ?? '',
    process.env.SystemDrive ?? 'C:',
  );

  return loginUsersPathOf(root, sandbox.steam);
}

/** Where Sandboxie puts its boxes, which is nothing at all on an installation nobody has moved. */
async function fileRootPath(sandbox: Sandbox): Promise<string> {
  try {
    const { stdout } = await run(sandbox.ini, ['query', 'GlobalSettings', 'FileRootPath']);

    return stdout;
  } catch {
    return '';
  }
}

/**
 * When Steam last wrote down that this account is signed in, or nothing where it has not.
 *
 * The when is what tells a sign-in from the memory of one: Steam rewrites this file every time it
 * signs in, so a file newer than the Steam that was just started is that Steam signing in, and one
 * older than it is what the box remembered from last time. Whether that is enough is
 * `signedInNowOf`'s to say; this only reads.
 */
async function signInWritten(users: string, account: string): Promise<number | undefined> {
  try {
    const written = await stat(users);

    return signedInOf(await readFile(users, 'utf8'), account) ? written.mtimeMs : undefined;
  } catch {
    // A box that has never had a Steam in it has no such file, which is simply "not yet".
    return undefined;
  }
}

function sleep(milliseconds: number): Promise<void> {
  return new Promise((resolve) => {
    setTimeout(resolve, milliseconds);
  });
}
