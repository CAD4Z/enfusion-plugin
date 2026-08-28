/**
 * Finding DayZ and DayZ Tools through Steam.
 *
 * The registry is asked first — both installers write their path there, and an answer out of it
 * costs one `reg` call. It is not always there to give one: a DayZ moved between library folders,
 * a Tools installed while the key was written by another user, a machine where the launcher has
 * never run leave the key stale or absent, and the panel that used to show that gap is gone. So
 * Steam is asked next, which is where both of them actually live.
 *
 * Steam keeps its library folders in one file — `steamapps\libraryfolders.vdf` under the
 * installation — and each library block lists the apps installed into it by id. That is enough to
 * pick the right library without opening every one of them; the app's own manifest beside it then
 * names the folder it was installed under, because `installdir` is not always the app's name.
 *
 * Nothing here goes near a disk: the files are handed in as text, and what they mean is worked out
 * here. The format is Valve's KeyValues — quoted strings, nested blocks, `//` comments — and only
 * as much of it as these two files use is read.
 */

import { windowsPath } from './paths';

/** The two apps a mod is built and run with, by the id Steam knows them under. */
export const STEAM_APP = {
  dayz: '221100',
  dayzTools: '830640',
} as const;

/** A block of a KeyValues file: fields, and the blocks under them. */
export interface KeyValues {
  readonly [key: string]: string | KeyValues;
}

/** Where Steam lists its library folders, under the installation the registry names. */
export function libraryFoldersPath(steam: string): string {
  return windowsPath(steam, 'steamapps', 'libraryfolders.vdf');
}

/** The app's own manifest, which sits beside the folder it was installed into. */
export function appManifestPath(library: string, appId: string): string {
  return windowsPath(library, 'steamapps', `appmanifest_${appId}.acf`);
}

/** The installation itself: `<library>\steamapps\common\<installdir>`. */
export function appPath(library: string, installDir: string): string {
  return windowsPath(library, 'steamapps', 'common', installDir);
}

/**
 * The library folder the app is installed into, or undefined where no library claims it. Each
 * block carries an `apps` list of what it holds, so the file answers this on its own — no library
 * has to be opened to find out that it is the wrong one.
 */
export function libraryOf(vdf: string, appId: string): string | undefined {
  return librariesOf(vdf).find((library) => library.apps.includes(appId))?.path;
}

/** One library folder as the file describes it: where it is, and what is installed into it. */
export interface SteamLibrary {
  readonly path: string;
  readonly apps: readonly string[];
}

/** The library folders, in the order the file lists them. */
export function librariesOf(vdf: string): SteamLibrary[] {
  const root = blockOf(parseKeyValues(vdf), 'libraryfolders');

  return Object.values(root ?? {}).flatMap((entry) => {
    if (typeof entry === 'string') {
      return [];
    }

    const path = entry.path;
    if (typeof path !== 'string' || path === '') {
      return [];
    }

    const apps = entry.apps;

    return [{ path, apps: typeof apps === 'object' ? Object.keys(apps) : [] }];
  });
}

/** The folder an app was installed under, which is not always what the app is called. */
export function installDirOf(acf: string): string | undefined {
  const installDir = blockOf(parseKeyValues(acf), 'AppState')?.installdir;

  return typeof installDir === 'string' && installDir !== '' ? installDir : undefined;
}

/**
 * A KeyValues file as a tree. Malformed input is read as far as it goes rather than thrown over:
 * a file Steam is halfway through writing is not worth losing an installation path over, and half
 * a tree answers the one question asked of it just as well as a whole one.
 */
export function parseKeyValues(source: string): KeyValues {
  const tokens = tokensOf(source);
  const root: Mutable = {};
  const stack: Mutable[] = [root];

  let key: string | undefined;

  for (const token of tokens) {
    const holder = stack[stack.length - 1];
    if (holder === undefined) {
      break;
    }

    if (token === '{') {
      // A block with no key in front of it belongs to nobody, so it is read and dropped.
      const block: Mutable = {};
      if (key !== undefined) {
        holder[key] = block;
      }
      key = undefined;
      stack.push(block);
      continue;
    }

    if (token === '}') {
      // Never past the root: a stray brace closes nothing rather than unbalancing everything after.
      if (stack.length > 1) {
        stack.pop();
      }
      key = undefined;
      continue;
    }

    if (key === undefined) {
      key = token;
      continue;
    }

    holder[key] = token;
    key = undefined;
  }

  return root;
}

/** The tree under a key, or undefined where the key holds a value or is not there at all. */
function blockOf(values: KeyValues, key: string): KeyValues | undefined {
  const found = values[key];

  return typeof found === 'object' ? found : undefined;
}

interface Mutable {
  [key: string]: string | Mutable;
}

/**
 * The file as braces and strings. Unquoted tokens are read too — Valve's own writers quote
 * everything, but its readers do not require it — and a `\` before anything stands for that thing,
 * which is how a Windows path survives being written into one of these.
 */
function tokensOf(source: string): string[] {
  const tokens: string[] = [];
  let at = 0;

  while (at < source.length) {
    const character = source[at] ?? '';

    if (character === '/' && source[at + 1] === '/') {
      at = endOfLine(source, at);
      continue;
    }

    if (/\s/.test(character)) {
      at += 1;
      continue;
    }

    if (character === '{' || character === '}') {
      tokens.push(character);
      at += 1;
      continue;
    }

    if (character === '"') {
      const quoted = quotedAt(source, at + 1);
      tokens.push(quoted.value);
      at = quoted.end;
      continue;
    }

    const bare = bareAt(source, at);
    tokens.push(bare.value);
    at = bare.end;
  }

  return tokens;
}

function endOfLine(source: string, at: number): number {
  const end = source.indexOf('\n', at);

  return end === -1 ? source.length : end + 1;
}

/** From just after the opening quote to just after the closing one, or to the end without one. */
function quotedAt(source: string, from: number): { value: string; end: number } {
  let value = '';

  for (let at = from; at < source.length; at += 1) {
    const character = source[at] ?? '';

    if (character === '\\') {
      value += source[at + 1] ?? '';
      at += 1;
      continue;
    }

    if (character === '"') {
      return { value, end: at + 1 };
    }

    value += character;
  }

  return { value, end: source.length };
}

function bareAt(source: string, from: number): { value: string; end: number } {
  let at = from;
  while (at < source.length && !/[\s{}"]/.test(source[at] ?? '')) {
    at += 1;
  }

  return { value: source.slice(from, at), end: at };
}
