/**
 * The box the second client is run in.
 *
 * Two clients on one machine are two Steam accounts, and Steam signs one account in per Windows
 * session — so the second client is run inside a Sandboxie box that has a Steam of its own. That
 * used to be four command lines in the settings, one of which had to name a box the other three
 * agreed with, and the developer had to keep all four right by hand. What is asked for now is the
 * account name: Sandboxie and Steam are found the way DayZ is found, the box is made if it is not
 * there, and everything below is worked out from those three paths.
 *
 * One box, named here rather than configured. A box is not a thing a developer picks between — it
 * is where the second Steam lives, and a second name for it would only be a second place for the
 * setup to be half done.
 *
 * As everywhere else in `src/mods`, nothing here goes near a disk or a process: these are the
 * command lines and the paths, and `src/platform/sandbox.ts` is what runs them.
 */

import type { SecondClient } from '../machine';
import { windowsPath } from '../paths';
import { parseKeyValues, type KeyValues } from './steam';

/** The box the second client runs in, and the Steam that signs its account in. */
export const BOX = 'steam2';

/** Sandboxie's two programs: the one that starts something in a box, and the one that configures. */
const START = 'Start.exe';
const SBIEINI = 'SbieIni.exe';

/** Steam, under the folder Steam itself recorded as its own. */
const STEAM = 'steam.exe';

/** Where Steam records who is signed in, counted from the folder Steam is installed in. */
const LOGIN_USERS: readonly string[] = ['config', 'loginusers.vdf'];

/** Steam writes account-name/SteamID pairs here even without a boxed loginusers.vdf. */
const STEAM_CONFIG: readonly string[] = ['config', 'config.vdf'];

/** Where Steam records the live connection state of the client. */
const CONNECTION_LOG: readonly string[] = ['logs', 'connection_log.txt'];

/** The preceding part of the connection log, retained when Steam rotates the live file. */
const PREVIOUS_CONNECTION_LOG: readonly string[] = ['logs', 'connection_log.previous.txt'];

/** A successful completion, including the local-time timestamp Steam writes at the start. */
const LOGGED_ON_COMPLETE = new RegExp(
  String.raw`^\[(\d{4})-(\d{2})-(\d{2}) (\d{2}):(\d{2}):(\d{2})\].*\[Logged On,[^\]\r\n]*\].*\[U:1:(\d+)\].*RecvMsgClientLogOnResponse\(\) : processing complete`,
  'gm',
);

/** Lines after a completed logon that mean it no longer describes the current client state. */
const NOT_CONNECTED: readonly string[] = [
  'Client version:',
  'RecvMsgClientLoggedOff(',
  'AsyncDisconnect(',
  'ConnectionDisconnected(',
  'LogOff()',
  '[Logged Off,',
  '[Logging On,',
  '[Logging Off,',
  'Log session ended',
];

/**
 * What Sandboxie calls the folder a box keeps its files in when nothing said otherwise. The
 * pattern is Sandboxie's own default for `FileRootPath`, and the `\??\` in front of it is the NT
 * way of writing a path rather than part of the folder.
 */
const DEFAULT_ROOT = '\\??\\%SystemDrive%\\Sandbox\\%USER%\\%SANDBOX%';

/** The box, and the programs a second client is put up with. */
export interface Sandbox {
  readonly box: string;
  /** Sandboxie's `Start.exe`, which is what puts a program inside the box. */
  readonly start: string;
  /** Sandboxie's `SbieIni.exe`, which is what makes the box and answers whether it is there. */
  readonly ini: string;
  /** Steam's folder — the second client's Steam is the same installation, sandboxed. */
  readonly steam: string;
  /** The account Steam signs in as. */
  readonly account: string;
}

/**
 * What a second client is started inside on this machine.
 *
 * Three answers rather than two, because a machine with no second account is not a machine that is
 * set up wrong: a second client is worth having there too — offline, in a window of its own — and
 * it is simply started the way the first one is.
 */
export type SandboxPlan =
  | { readonly kind: 'none' }
  | { readonly kind: 'box'; readonly sandbox: Sandbox }
  | { readonly kind: 'wanting'; readonly said: string };

/**
 * The box the settings and the machine between them describe.
 *
 * The account is what says whether any of this is wanted at all: named, the second client wants a
 * Steam of its own and everything else has to be there for it; left empty, it is another client
 * and nothing is looked for. Sandboxie and Steam are found the way every other program is — the
 * setting, and the registry behind it — so a machine that has both needs neither typed.
 */
export function sandboxPlanOf(second: SecondClient): SandboxPlan {
  if (second.account === '') {
    return { kind: 'none' };
  }

  if (second.sandboxie === '') {
    return {
      kind: 'wanting',
      said:
        'A second client that signs in as another Steam account is run inside a Sandboxie box, ' +
        'and Sandboxie was not found: install Sandboxie-Plus, or set enfusion.sandboxie.path. ' +
        'Clearing enfusion.launch.secondAccount makes the second client simply another client.',
    };
  }

  if (second.steam === '') {
    return {
      kind: 'wanting',
      said:
        'Steam was not found, and a second client is a Steam of its own inside a box: set ' +
        'enfusion.steam.path to the folder holding steam.exe.',
    };
  }

  return {
    kind: 'box',
    sandbox: {
      box: BOX,
      start: windowsPath(second.sandboxie, START),
      ini: windowsPath(second.sandboxie, SBIEINI),
      steam: second.steam,
      account: second.account,
    },
  };
}

/** Steam itself, which is what the box has to have signed in before the game is started in it. */
export function steamExecutableOf(sandbox: Sandbox): string {
  return windowsPath(sandbox.steam, STEAM);
}

/** What puts a program inside the box, argument by argument, with the program after it. */
export function boxPrefixOf(sandbox: Sandbox): string[] {
  return [sandbox.start, `/box:${sandbox.box}`];
}

/**
 * The same, for the game — with `/wait`, which is the whole difference and not a small one.
 *
 * `Start.exe` hands the program to Sandboxie and exits, so without it the process the extension
 * holds is gone a moment after the game comes up: the session says the second client is gone while
 * it is playing, and Stop kills something that has already exited instead of the game. Told to
 * wait, it lives exactly as long as what it started, which is what makes it stand for it.
 */
export function gamePrefixOf(sandbox: Sandbox): string[] {
  return [...boxPrefixOf(sandbox), '/wait'];
}

/**
 * The Steam that has to be up in the box before the game is.
 *
 * `-login` names the account rather than switching to it: a box that has signed in before signs in
 * again by itself, and a box that has not shows its login window with the name already in it.
 * `-silent` keeps it out of the way once it is up — what was asked for is a client, not a Steam
 * window in front of the game that is already playing. `-inhibitbootstrap` is deliberately the
 * one undocumented switch here: a Steam sharing its installation with the unboxed Steam cannot
 * replace those files while that instance is running. Letting it try leaves a mixed sandbox copy,
 * followed by a checksum pass and rollback; the host installation is the one that updates them.
 */
export function steamCommandOf(sandbox: Sandbox): string[] {
  return [
    steamExecutableOf(sandbox),
    '-login',
    sandbox.account,
    '-silent',
    '-inhibitbootstrap',
  ];
}

/**
 * What a box of ours is made with, in the order it is written.
 *
 * `Enabled` is what makes the box: writing it is what puts the section into Sandboxie's own
 * configuration, and Sandboxie fills a new box in with its defaults itself — the templates, the
 * recovery folders, the border. So this is not a box configured from here; it is the box Sandboxie
 * would have made, plus the two things a developer would otherwise have to remember.
 *
 * `NeverRemove` is the one that matters. It is what Sandboxie refuses to delete a box over, and
 * what is inside this one is a signed-in Steam: delete it and the next launch asks for the
 * password and the Steam Guard code again. `AutoDelete` is the same answer to the other way of
 * losing it — a box that empties itself every time it is closed is a box that never remembers a
 * sign-in — and it is written even though `n` is the default, because it being `y` is paid for by
 * the developer and not by this.
 *
 * Written when the box is made and never again: a box that is there is the developer's, and what
 * they chose in the Sandboxie window for it is not this extension's to write over.
 */
export const BOX_SETTINGS: readonly (readonly [string, string])[] = [
  ['Enabled', 'y'],
  ['NeverRemove', 'y'],
  ['AutoDelete', 'n'],
];

/** Asked of `SbieIni query <box> Enabled`, which answers with nothing for a box that is not there. */
export function boxExistsOf(stdout: string): boolean {
  return stdout.trim() !== '';
}

/**
 * The processes in the box, out of `Start.exe /box:<box> /listpids`: a count, and then one pid a
 * line. The count is dropped rather than read — what is wanted is the pids.
 *
 * That the count is not the answer is the whole reason this is read at all. A box that has run
 * anything keeps two or three services of Sandboxie's own alive in it, so "is there anything in
 * the box" says yes to an empty box, and it said yes to one for long enough to be worth writing
 * down. What is asked instead is whether one of these is Steam.
 */
export function boxPidsOf(stdout: string): number[] {
  const numbers = stdout
    .split(/\r?\n/)
    .map((line) => line.trim())
    .filter((line) => /^\d+$/.test(line))
    .map(Number);

  return numbers.slice(1);
}

/**
 * The pids of one program, out of `tasklist /FI "IMAGENAME eq <image>" /NH /FO CSV`.
 *
 * CSV and not the table it prints by default, because the table is drawn in columns whose headings
 * are in the language Windows was installed in, and the line it prints when nothing matched is a
 * sentence in that language too. A quoted row is neither: a line that is not a process is a line
 * this does not match.
 */
export function imagePidsOf(stdout: string): number[] {
  return [...stdout.matchAll(/^"[^"]*","(\d+)"/gm)].map((match) => Number(match[1]));
}

/**
 * The folder the box keeps its files in: whatever `FileRootPath` says, with the three things
 * Sandboxie writes into that pattern filled in. Nothing said is Sandboxie's own default, which is
 * what an installation nobody has configured has.
 *
 * A pattern with no `%SANDBOX%` in it names a folder every box shares, and Sandboxie puts the box
 * under it — so this does too, rather than handing back one folder for all of them.
 */
export function boxRootOf(
  fileRootPath: string,
  box: string,
  user: string,
  systemDrive: string,
): string {
  const pattern = fileRootPath.trim() === '' ? DEFAULT_ROOT : fileRootPath.trim();
  const filled = pattern
    .replace(/^\\\?\?\\/, '')
    .replace(/%SystemDrive%/gi, systemDrive)
    .replace(/%USER%/gi, user)
    .replace(/%SANDBOX%/gi, box);

  return /%SANDBOX%/i.test(pattern) ? filled : windowsPath(filled, box);
}

/**
 * Where the box's own copy of Steam's `loginusers.vdf` is.
 *
 * A sandboxed program writes into a mirror of the disk under the box, one folder per drive letter,
 * so Steam's `C:\...\Steam\config\loginusers.vdf` is the box's `drive\C\...\Steam\config\`. Which
 * is what makes the sign-in readable from outside the box at all — and the sign-in is the one
 * thing a launch has to wait for.
 */
export function loginUsersPathOf(boxRoot: string, steam: string): string | undefined {
  return steamFileInBoxOf(boxRoot, steam, ...LOGIN_USERS);
}

/** The boxed installation config, another record of account names and their SteamIDs. */
export function steamConfigPathOf(boxRoot: string, steam: string): string | undefined {
  return steamFileInBoxOf(boxRoot, steam, ...STEAM_CONFIG);
}

/** Where the boxed Steam appends its live connection state. */
export function connectionLogPathOf(boxRoot: string, steam: string): string | undefined {
  return steamFileInBoxOf(boxRoot, steam, ...CONNECTION_LOG);
}

/** Where Steam keeps the preceding segment when it rotates the boxed connection log. */
export function previousConnectionLogPathOf(boxRoot: string, steam: string): string | undefined {
  return steamFileInBoxOf(boxRoot, steam, ...PREVIOUS_CONNECTION_LOG);
}

function steamFileInBoxOf(
  boxRoot: string,
  steam: string,
  ...file: readonly string[]
): string | undefined {
  return inBoxOf(boxRoot, windowsPath(steam, ...file));
}

/**
 * Where the box keeps its own copy of a file: under `drive\<letter>`, the rest of the path as it
 * is. A sandboxed program reads that copy once there is one, and the file on the disk until then.
 *
 * The path has to be the real one. A box files what is on a `subst` drive under the drive it is
 * mapped from — `P:\Profiles` of a work drive mounted from `F:\Code\DayZ\PDrive` is kept under
 * `drive\F\Code\DayZ\PDrive\Profiles` — so a path through the mount is resolved before it is asked.
 */
export function inBoxOf(boxRoot: string, path: string): string | undefined {
  const drive = /^([A-Za-z]):[\\/]?(.*)$/.exec(path);
  if (drive === null) {
    return undefined;
  }

  return windowsPath(boxRoot, 'drive', (drive[1] ?? '').toUpperCase(), drive[2] ?? '');
}

/**
 * The Steam3 account id belonging to a remembered account.
 *
 * `loginusers.vdf` files each account under its SteamID64. The low 32 bits are the account id in
 * the `[U:1:<id>]` Steam3 form used by the live connection log. A first boxed sign-in can leave
 * that file absent while `config.vdf` already records the same mapping under Steam/Accounts.
 */
export function steamAccountIdOf(vdf: string, account: string): string | undefined {
  const wanted = account.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');

  for (const user of vdf.matchAll(/^\s*"(\d+)"\s*\{([^{}]*)\}/gm)) {
    const fields = user[2] ?? '';
    if (new RegExp(`"AccountName"\\s*"${wanted}"`, 'i').test(fields)) {
      const steamId = user[1];
      if (steamId !== undefined) {
        return (BigInt(steamId) & 0xffffffffn).toString();
      }
    }
  }

  let field: KeyValues | string | undefined = parseKeyValues(vdf);
  for (const key of ['InstallConfigStore', 'Software', 'Valve', 'Steam', 'Accounts', account, 'SteamID']) {
    if (typeof field !== 'object') {
      return undefined;
    }
    field = Object.entries(field).find(([name]) => name.toLowerCase() === key.toLowerCase())?.[1];
  }

  return typeof field === 'string' && /^\d+$/.test(field)
    ? (BigInt(field) & 0xffffffffn).toString()
    : undefined;
}

/**
 * How long the second client waits for the sign-in before handing the console back.
 *
 * Long, because the first sign-in into a fresh box is a password and a Steam Guard code out of an
 * email, and a developer typing those is doing exactly what this is waiting for. Not endless,
 * because the button is held by the wait: giving up says what is missing and leaves the box up, so
 * the press after it finds a Steam that is already there and goes straight to the game.
 */
export const SIGN_IN_PATIENCE = 5 * 60 * 1000;

/**
 * Whether this Steam process, rather than a preceding one, completed a successful logon.
 *
 * Steam's log timestamps have one-second precision. The process start is therefore rounded down
 * to the same precision before they are compared. Reading the rotated predecessor before the live
 * file keeps a still-connected session recognisable after `connection_log.txt` is rotated.
 */
export function steamConnectedSinceOf(
  log: string,
  processStartedAt: number,
  accountId: string,
): boolean {
  const connection = currentConnectionOf(log);
  const processSecond = Math.floor(processStartedAt / 1000) * 1000;

  return (
    connection !== undefined && connection.at >= processSecond && connection.accountId === accountId
  );
}

interface SteamConnection {
  readonly accountId: string;
  readonly at: number;
  readonly offset: number;
}

function currentConnectionOf(log: string): SteamConnection | undefined {
  let connection: SteamConnection | undefined;

  for (const match of log.matchAll(LOGGED_ON_COMPLETE)) {
    const at = new Date(
      Number(match[1]),
      Number(match[2]) - 1,
      Number(match[3]),
      Number(match[4]),
      Number(match[5]),
      Number(match[6]),
    ).getTime();

    const accountId = match[7];
    if (!Number.isNaN(at) && match.index !== undefined && accountId !== undefined) {
      connection = { accountId, at, offset: match.index };
    }
  }

  if (connection === undefined) {
    return undefined;
  }

  const disconnected = Math.max(...NOT_CONNECTED.map((marker) => log.lastIndexOf(marker)));

  return connection.offset > disconnected ? connection : undefined;
}
