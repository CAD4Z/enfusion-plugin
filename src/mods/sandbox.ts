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

import type { SecondClient } from './machine';
import { windowsPath } from './paths';

/** The box the second client runs in, and the Steam that signs its account in. */
export const BOX = 'steam2';

/** Sandboxie's two programs: the one that starts something in a box, and the one that configures. */
const START = 'Start.exe';
const SBIEINI = 'SbieIni.exe';

/** Steam, under the folder Steam itself recorded as its own. */
const STEAM = 'steam.exe';

/** Where Steam records who is signed in, counted from the folder Steam is installed in. */
const LOGIN_USERS: readonly string[] = ['config', 'loginusers.vdf'];

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
 * window in front of the game that is already playing.
 */
export function steamCommandOf(sandbox: Sandbox): string[] {
  return [steamExecutableOf(sandbox), '-login', sandbox.account, '-silent'];
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
  const drive = /^([A-Za-z]):[\\/]?(.*)$/.exec(steam);
  if (drive === null) {
    return undefined;
  }

  return windowsPath(
    boxRoot,
    'drive',
    (drive[1] ?? '').toUpperCase(),
    drive[2] ?? '',
    ...LOGIN_USERS,
  );
}

/**
 * Whether Steam's own record of who is signed in names the account.
 *
 * Steam writes this file when a sign-in succeeds and not before, which is what makes it the answer
 * to "has the developer finished typing the password" — a question nothing else here can ask. The
 * file is a Valve key-value tree; what is wanted out of it is one name, so it is looked for rather
 * than parsed.
 */
export function signedInOf(vdf: string, account: string): boolean {
  const wanted = account.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');

  return new RegExp(`"AccountName"\\s*"${wanted}"`, 'i').test(vdf);
}

/**
 * How long a Steam that was started here is given to sign itself in before the record it left last
 * time is taken as the answer.
 *
 * Which is the hedge, and it is worth being plain about what it hedges. The record is rewritten on
 * every successful sign-in — that is what dates it, and what tells this sign-in from the memory of
 * the last one. If some build of Steam ever signs in without touching it, the alternative to a
 * grace period is a launch that waits the full five minutes every single time and then says the
 * account is not signed in while it plainly is. Long enough that a login window is still being
 * typed into when it passes, short enough that nobody sits through it.
 */
export const SIGN_IN_GRACE = 45 * 1000;

/** A first sign-in includes a password and often Steam Guard, so it gets five full minutes. */
export const SIGN_IN_PATIENCE = 5 * 60 * 1000;

/**
 * Whether the account counts as signed in: named in what Steam wrote, and either written since the
 * Steam that is being waited for was started or old enough not to be worth doubting.
 *
 * `written` is when the record was last written, or nothing at all for a box that has never had a
 * Steam in it. `since` is when that Steam was started, and nothing to have started — a Steam that
 * was already up when this began — is a zero, which every record is newer than.
 */
export function signedInNowOf(written: number | undefined, since: number, now: number): boolean {
  return written !== undefined && (written >= since || now - since >= SIGN_IN_GRACE);
}
