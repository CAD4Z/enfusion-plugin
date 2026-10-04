/**
 * The window a client comes up in: borderless, covering the primary monitor, with nothing of it off
 * the screen — made by the game itself, out of its own settings.
 *
 * DayZ has three window modes, and the one wanted here is the one its options menu never names.
 * `Windowed=1` at a resolution smaller than the desktop's is a window with a title bar, put at
 * `WinX`/`WinY` — the corner of the frame rather than of the picture, so a window as large as the
 * monitor comes up with its title bar on the screen and the bottom of the picture off it.
 * `Windowed=1` at exactly the desktop's resolution is a borderless popup maximised over the whole
 * monitor, which the engine logs as `bordeless`. `Windowed=0` is exclusive fullscreen.
 *
 * `-window` on the command line forces the first of the three whatever the settings say, and is
 * what put the title bar on the screen; it is not passed. The settings are put right instead,
 * before every start: `Windowed=1`, the window at the primary monitor's corner, and no resolution at
 * all — a game given none takes the primary display's own, which is exactly what makes it choose
 * the borderless mode. That is also why the resolution is taken out rather than written: the game
 * reads the desktop in physical pixels, and nothing here has to know which monitor or what scale.
 *
 * A settings file that is not there yet is written too. Without one the game is not windowed: a
 * missing `Windowed` is read as exclusive fullscreen, which is no way to share a screen with the
 * editor that started it.
 *
 * None of this is documented; it was read out of DayZDiag_x64 1.29. The settings are loaded before
 * the window is made — the first backbuffer of every run is whatever resolution the last one left
 * in the file — and a resolution missing from the file defaults to the primary display's current
 * mode.
 */

import { windowsPath } from '../paths';

/** Where a profile keeps the settings of the Windows account the game runs under. */
const USERS_FOLDER = 'Users';

/** The file the game keeps its display settings in, which Experimental names after itself. */
const SETTINGS_FILE = { stable: 'DayZ.cfg', experimental: 'DayZ Exp.cfg' } as const;

/** What the borderless window needs said, in the spelling the game writes them in. */
const WRITTEN: readonly (readonly [string, string])[] = [
  ['Windowed', '1'],
  ['WinX', '0'],
  ['WinY', '0'],
];

/** And what it needs left unsaid, so that the game falls back to the desktop's resolution. */
const DROPPED: readonly string[] = ['windowwidth', 'windowheight'];

/**
 * The settings file a client reads its window from: in the profile it is started with, under the
 * Windows account the game runs as — not under `-name`, which files the player's own profile.
 */
export function windowSettingsOf(profile: string, user: string, experimental: boolean): string {
  return windowsPath(
    profile,
    USERS_FOLDER,
    user,
    experimental ? SETTINGS_FILE.experimental : SETTINGS_FILE.stable,
  );
}

/**
 * The settings as the borderless window wants them, and everything else in them — the language,
 * the graphics the developer picked — left exactly as it was. Keys are matched the way the game
 * matches them, whatever their case; one said twice is said once.
 */
export function borderlessSettingsOf(text: string): string {
  const newline = text === '' || text.includes('\r\n') ? '\r\n' : '\n';
  const lines = text.split(/\r?\n/);
  if (lines.at(-1) === '') {
    lines.pop();
  }

  const said = new Set<string>();
  const kept: string[] = [];

  for (const line of lines) {
    const key = keyOf(line);
    const written = WRITTEN.find(([name]) => name.toLowerCase() === key);

    if (key !== undefined && DROPPED.includes(key)) {
      continue;
    }

    if (written === undefined) {
      kept.push(line);
    } else if (!said.has(written[0])) {
      said.add(written[0]);
      kept.push(entryOf(written));
    }
  }

  for (const written of WRITTEN) {
    if (!said.has(written[0])) {
      kept.push(entryOf(written));
    }
  }

  return `${kept.join(newline)}${newline}`;
}

function keyOf(line: string): string | undefined {
  return /^\s*([A-Za-z_][A-Za-z0-9_]*)\s*=/.exec(line)?.[1]?.toLowerCase();
}

function entryOf([name, value]: readonly [string, string]): string {
  return `${name}=${value};`;
}
