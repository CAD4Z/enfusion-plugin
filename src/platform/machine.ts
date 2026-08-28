/**
 * The machine side of the settings: what the editor holds, what the registry answers, and which of
 * the paths are actually on disk. What any of it means is decided in `src/mods/machine.ts`.
 *
 * A setting the developer left empty is filled in from the registry, where the installers wrote
 * the paths in the first place — the ordinary machine needs nothing typed at all. A key that is
 * not there is an empty value: `reg` exits non-zero for a missing key just as it does for a
 * missing registry, and neither is worth failing a scan over.
 *
 * DayZ and DayZ Tools have a second place to be found: Steam, which is where both of them are
 * installed and which knows where it put them. It is asked when the registry answered with a path
 * nothing is at — a library folder the app was moved out of, a key another user wrote — because a
 * stale answer and no answer are worth the same to a build. What the developer typed is never
 * second-guessed this way: a setting that is filled in is the last word on where the thing is.
 */

import { execFile } from 'node:child_process';
import { promisify } from 'node:util';
import * as vscode from 'vscode';
import {
  type EnvironmentEntry,
  type MachineSettings,
  SECTION,
  SETTING,
  builderOf,
  environmentOf,
  environmentPaths,
} from '../mods/machine';
import { registryValue } from '../mods/registry';
import {
  STEAM_APP,
  appManifestPath,
  appPath,
  installDirOf,
  libraryFoldersPath,
  libraryOf,
} from '../mods/steam';

const run = promisify(execFile);

/**
 * The settings as the extension acts on them, with the registry standing in for what is empty.
 * The registry is asked once per session and remembered, `reread` being what a developer who has
 * just installed something asks for by pressing refresh.
 */
export async function readMachineSettings(reread = false): Promise<MachineSettings> {
  if (reread) {
    answered.clear();
    steamed.clear();
  }

  const settings = vscode.workspace.getConfiguration();
  const text = (id: string): string => {
    const value: unknown = settings.get(id);
    return typeof value === 'string' ? value.trim() : '';
  };

  return {
    dayz: text(SETTING.dayz) || (await installed(DAYZ, STEAM_APP.dayz)),
    executable: text(SETTING.executable),
    dayzTools: text(SETTING.dayzTools) || (await installed(DAYZ_TOOLS, STEAM_APP.dayzTools)),
    pboProject: text(SETTING.pboProject) || (await fromRegistry(PBOPROJECT)),
    privateKey: text(SETTING.privateKey),
    workDrive: text(SETTING.workDrive),
    workDriveLetter: text(SETTING.workDriveLetter),
    filePatchingRoot: text(SETTING.filePatchingRoot),
    profiles: text(SETTING.profiles),
    builder: builderOf(text(SETTING.builder)),
  };
}

/** What resolved and what did not, which is what the panel shows before the first build. */
export async function readEnvironment(settings: MachineSettings): Promise<EnvironmentEntry[]> {
  const found = await Promise.all(
    environmentPaths(settings).map(async (path) => ((await exists(path)) ? [path] : [])),
  );

  return environmentOf(settings, found.flat());
}

/** Calls back whenever a setting of ours changes, since every one of them shows on the panel. */
export function watchMachineSettings(onChange: () => void): vscode.Disposable {
  return vscode.workspace.onDidChangeConfiguration((event) => {
    if (event.affectsConfiguration(SECTION)) {
      onChange();
    }
  });
}

/** A registry value, by the key and the name the installer wrote it under. */
interface RegistryValue {
  readonly key: string;
  readonly name: string;
}

/** Where DayZ records its installation, the likeliest place first. */
const DAYZ: readonly RegistryValue[] = [
  { key: 'HKLM\\SOFTWARE\\WOW6432Node\\Bohemia Interactive\\DayZ', name: 'main' },
  { key: 'HKLM\\SOFTWARE\\Bohemia Interactive\\DayZ', name: 'main' },
  { key: 'HKCU\\SOFTWARE\\Bohemia Interactive\\DayZ', name: 'main' },
];

/** DayZ Tools writes its own path per user, and the launcher writes a machine-wide copy. */
const DAYZ_TOOLS: readonly RegistryValue[] = [
  { key: 'HKCU\\SOFTWARE\\Bohemia Interactive\\DayZ Tools', name: 'path' },
  { key: 'HKLM\\SOFTWARE\\WOW6432Node\\Bohemia Interactive\\DayZ Tools', name: 'path' },
  { key: 'HKLM\\SOFTWARE\\Bohemia Interactive\\DayZ Tools', name: 'path' },
];

/**
 * Mikero's installer records the executable rather than the folder it sits in, per user and
 * nowhere else — which is why the setting behind this one names a file and not a directory.
 */
const PBOPROJECT: readonly RegistryValue[] = [
  { key: 'HKCU\\SOFTWARE\\Mikero\\pboProject', name: 'exe' },
];

/**
 * Where Steam is, which is the way to everything Steam installed. The per-user key holds it in
 * forward slashes and lower case, the machine-wide one the way Windows writes a path; both are
 * the same folder, and neither is a folder anything here cares about beyond reading one file in.
 */
const STEAM: readonly RegistryValue[] = [
  { key: 'HKCU\\SOFTWARE\\Valve\\Steam', name: 'SteamPath' },
  { key: 'HKLM\\SOFTWARE\\WOW6432Node\\Valve\\Steam', name: 'InstallPath' },
  { key: 'HKLM\\SOFTWARE\\Valve\\Steam', name: 'InstallPath' },
];

/**
 * Where an app is, out of the registry first and Steam behind it. The registry's answer wins while
 * it points at something; a path nothing is at is worth no more than no path, so Steam is asked
 * before it is settled for — and it is settled for in the end, because a wrong path a developer
 * can see in the log beats an empty one that says nothing at all.
 */
async function installed(candidates: readonly RegistryValue[], appId: string): Promise<string> {
  const recorded = await fromRegistry(candidates);
  if (recorded !== '' && (await exists(recorded))) {
    return recorded;
  }

  const found = await fromSteam(appId);

  return found === '' ? recorded : found;
}

/**
 * The app's folder as Steam has it: the library its own list says holds the app, and the folder
 * name the app's manifest says it was installed under — `installdir` is not always the app's name.
 * Every step is a file that may not be there, and any of them missing means simply "not through
 * Steam, then".
 */
async function fromSteam(appId: string): Promise<string> {
  const remembered = steamed.get(appId);
  if (remembered !== undefined) {
    return remembered;
  }

  const found = await lookUpSteam(appId);
  steamed.set(appId, found);

  return found;
}

async function lookUpSteam(appId: string): Promise<string> {
  const steam = windowsish(await fromRegistry(STEAM));
  if (steam === '') {
    return '';
  }

  const library = libraryOf(await readText(libraryFoldersPath(steam)), appId);
  if (library === undefined) {
    return '';
  }

  const installDir = installDirOf(await readText(appManifestPath(library, appId)));

  return installDir === undefined ? '' : appPath(library, installDir);
}

/** Steam writes its own path in forward slashes; everything downstream of here is a Windows path. */
function windowsish(path: string): string {
  return path.replace(/\//g, '\\');
}

/** A file that is not there reads as empty, which every reader here treats as "nothing said". */
async function readText(path: string): Promise<string> {
  try {
    return new TextDecoder().decode(await vscode.workspace.fs.readFile(vscode.Uri.file(path)));
  } catch {
    return '';
  }
}

/**
 * An answer, once given, holds until a refresh asks again: an installation does not move under the
 * editor, and a key that is not there stays not there — spawning `reg` for it on every file event
 * would be the same answer at a price.
 */
const answered = new Map<string, string>();

/** The same, for what Steam answered: three file reads per app, and the same answer every time. */
const steamed = new Map<string, string>();

async function fromRegistry(candidates: readonly RegistryValue[]): Promise<string> {
  if (process.platform !== 'win32') {
    return '';
  }

  for (const candidate of candidates) {
    const remembered = answered.get(idOf(candidate));
    const value = remembered ?? registryValue(await query(candidate), candidate.name);
    answered.set(idOf(candidate), value);

    if (value !== '') {
      return value;
    }
  }

  return '';
}

function idOf(value: RegistryValue): string {
  return `${value.key}\\${value.name}`;
}

async function query(value: RegistryValue): Promise<string> {
  try {
    const { stdout } = await run('reg', ['query', value.key, '/v', value.name]);
    return stdout;
  } catch {
    // Which is what `reg` does for a key that is not there, and the answer is simply "nothing".
    return '';
  }
}

/** Both a folder and a file count: what is asked is whether the setting points at anything. */
async function exists(path: string): Promise<boolean> {
  try {
    await vscode.workspace.fs.stat(vscode.Uri.file(path));
    return true;
  } catch {
    return false;
  }
}
