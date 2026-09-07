/**
 * Names that become folders.
 *
 * An owned mod has the narrowest grammar of everything it names: it is a folder, a CfgMods dir
 * and an Enforce class fragment. A loaded mod is somebody else's folder under `modsDirectory` and
 * has a wider grammar, but is still exactly one Windows path segment. Neither kind is cleaned up:
 * a value is accepted as written or refused, so the UI can keep showing the text that is wrong.
 *
 * The brands are capabilities. Code that builds a mutable path cannot pass an arbitrary manifest
 * string to `modPathOf`; it first has to obtain the corresponding checked name here.
 */

import { windowsPath } from './paths';

declare const MOD_NAME: unique symbol;
declare const LOADED_MOD_NAME: unique symbol;

/** A checked name belonging to a mod of this workspace. */
export type ModName = string & { readonly [MOD_NAME]: true };

/** A checked folder name from `clientMods` or `serverMods`, without its optional leading `@`. */
export type LoadedModName = string & { readonly [LOADED_MOD_NAME]: true };

/** Kept as text because the JSON schema is a deliberate static copy of the same rule. */
export const MOD_NAME_PATTERN = '^[A-Za-z_][A-Za-z0-9_]*$';

/** Also copied into the schema: these stay devices even when Windows is asked for a folder. */
export const WINDOWS_RESERVED_NAME_PATTERN =
  '^(?:[Cc][Oo][Nn]|[Pp][Rr][Nn]|[Aa][Uu][Xx]|[Nn][Uu][Ll]|' +
  '[Cc][Oo][Mm][1-9]|[Ll][Pp][Tt][1-9])(?:\\.|$)';

const MOD_NAME_RE = new RegExp(MOD_NAME_PATTERN);
const WINDOWS_RESERVED_NAME_RE = new RegExp(WINDOWS_RESERVED_NAME_PATTERN);
const INVALID_LOADED_MOD_CHARACTERS = '<>:"/\\|?*;';

/** The owned mod name when it is usable everywhere that name goes. */
export function modNameOf(raw: string): ModName | undefined {
  return MOD_NAME_RE.test(raw) && !windowsReserved(raw) ? (raw as ModName) : undefined;
}

/** Whether a prepared capability still belongs to the raw name carried beside it. */
export function isModNameOf(raw: string, checked: ModName | undefined): checked is ModName {
  return checked !== undefined && checked === modNameOf(raw);
}

/** Why an owned mod name cannot be used, or undefined when it can. */
export function modNameProblemOf(raw: string): string | undefined {
  if (raw.trim() === '') {
    return 'A mod needs a name: it is the folder it is linked and loaded under.';
  }

  if (!MOD_NAME_RE.test(raw)) {
    return (
      'A mod is named by a class as well as by a folder: letters, digits and underscores, ' +
      'starting with a letter or underscore.'
    );
  }

  return windowsReserved(raw)
    ? `${JSON.stringify(raw)} is a reserved Windows device name, not a folder name.`
    : undefined;
}

/**
 * A third-party mod reference as the one folder below `modsDirectory` it names.
 *
 * Spaces and hyphens are ordinary here: unlike an owned Mod name this is not written into a
 * class. Separators, Windows-invalid characters and the semicolon used to join `-mod=` paths are
 * not a folder name and are refused. The optional conventional `@` is a projection, not identity,
 * so the checked value does not carry it.
 */
export function loadedModNameOf(raw: string): LoadedModName | undefined {
  const name = raw.startsWith('@') ? raw.slice(1) : raw;

  return loadedModNameProblem(raw, name) === undefined ? (name as LoadedModName) : undefined;
}

/** Why a loaded-mod folder reference cannot be used, or undefined when it can. */
export function loadedModNameProblemOf(raw: string): string | undefined {
  const name = raw.startsWith('@') ? raw.slice(1) : raw;

  return loadedModNameProblem(raw, name);
}

function loadedModNameProblem(raw: string, name: string): string | undefined {
  if (name === '' || name === '.' || name === '..') {
    return 'A loaded mod needs one folder name under the mods directory.';
  }

  if (
    [...name].some(
      (character) =>
        INVALID_LOADED_MOD_CHARACTERS.includes(character) || character.charCodeAt(0) < 32,
    ) ||
    /[. ]$/.test(name) ||
    windowsReserved(name)
  ) {
    return (
      `"${raw}" is not one Windows folder name under the mods directory: ` +
      'slashes, path punctuation, control characters, semicolons and trailing dots or spaces ' +
      'cannot be used.'
    );
  }

  return undefined;
}

/** Windows device names stay reserved even when a file extension is written after them. */
function windowsReserved(name: string): boolean {
  return WINDOWS_RESERVED_NAME_RE.test(name);
}

/** A source name is a direct child; a built name is that same checked name with the conventional @. */
export function modPathOf(parent: string, name: ModName, kind: 'source' | 'built'): string;
export function modPathOf(parent: string, name: LoadedModName, kind: 'built'): string;
export function modPathOf(
  parent: string,
  name: ModName | LoadedModName,
  kind: 'built',
): string;
export function modPathOf(
  parent: string,
  name: ModName | LoadedModName,
  kind: 'source' | 'built',
): string {
  return windowsPath(parent, kind === 'built' ? `@${name}` : name);
}
