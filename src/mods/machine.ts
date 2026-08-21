/**
 * What belongs to the machine rather than to the mod.
 *
 * Where DayZ is installed, where DayZ Tools is, which key signs the pbo, which folder the work
 * drive is mounted from, where the file patching root is built, which builder packs: none of it
 * says anything about the mod, and a file under git is the wrong place for any of it — the private
 * key most of all. So it lives in the VS Code settings, contributed with `scope: machine`, which
 * the editor physically refuses to write into a workspace. See
 * `docs/adr/0002-enf-is-the-only-project-configuration.md`.
 *
 * In the ordinary case none of it is typed at all: the paths to DayZ and DayZ Tools are what the
 * installers wrote to the registry, and a missing entry is an empty value rather than a failure.
 * What the settings and the registry between them could not answer is what the panel shows, so
 * that a refusal is visible before the first build rather than during it.
 */

import { samePath, windowsPath } from './paths';

/** The program that packs an addon into a pbo. */
export type Builder = 'pboProject' | 'AddonBuilder';

export const BUILDERS: readonly Builder[] = ['pboProject', 'AddonBuilder'];

/** Everything the machine holds, already resolved: settings first, registry behind them. */
export interface MachineSettings {
  /** The DayZ installation — the folder the client and the diag executable sit in. */
  readonly dayz: string;
  readonly dayzTools: string;
  /** `pboProject.exe` itself, which is what its installer records rather than a folder. */
  readonly pboProject: string;
  /** The `.biprivatekey` to sign with; empty means the pbo goes unsigned. */
  readonly privateKey: string;
  /** The folder the work drive is mounted from. */
  readonly workDrive: string;
  /** The letter it is mounted under, however it was typed; see `driveLetterOf`. */
  readonly workDriveLetter: string;
  /** Where the file patching root is built; empty leaves the extension to pick the place. */
  readonly filePatchingRoot: string;
  readonly builder: Builder;
}

/** The settings, by the ids the editor knows them under, so the panel can open the right one. */
export const SETTING = {
  dayz: 'enfusion.dayz.path',
  dayzTools: 'enfusion.dayzTools.path',
  privateKey: 'enfusion.signing.privateKey',
  workDrive: 'enfusion.workDrive.source',
  workDriveLetter: 'enfusion.workDrive.letter',
  filePatchingRoot: 'enfusion.filePatching.root',
  builder: 'enfusion.builder',
  pboProject: 'enfusion.pboProject.path',
} as const;

/** The section every one of them sits under. */
export const SECTION = 'enfusion';

/**
 * What differs between the two builders as far as the machine is concerned: where each one is
 * found, which setting fills that in, and what to say when it is not there. One table, so that a
 * third builder is one entry rather than a hunt through the ternaries it would otherwise be.
 */
const BUILDER: Readonly<
  Record<
    Builder,
    {
      /** The executable, or empty where the machine has no answer for it. */
      readonly executable: (settings: MachineSettings) => string;
      /** The setting that would fill it in. */
      readonly setting: string;
      readonly missing: string;
    }
  >
> = {
  // pboProject's installer records the executable itself, so the setting names a file.
  pboProject: {
    executable: (settings) => settings.pboProject,
    setting: SETTING.pboProject,
    missing: 'pboProject was not found: install Mikero’s tools, or set enfusion.pboProject.path.',
  },
  // AddonBuilder comes with DayZ Tools and is found under wherever those are.
  AddonBuilder: {
    executable: (settings) =>
      settings.dayzTools === ''
        ? ''
        : windowsPath(settings.dayzTools, 'Bin', 'AddonBuilder', 'AddonBuilder.exe'),
    setting: SETTING.dayzTools,
    missing: 'AddonBuilder was not found: it comes with DayZ Tools, and no path to those is set.',
  },
};

/**
 * The program that will do the packing: whichever of the two the settings chose, at wherever the
 * machine has it. Empty means it was not found, which the panel shows as a gap and a build refuses
 * over.
 */
export function builderExecutableOf(settings: MachineSettings): string {
  return BUILDER[settings.builder].executable(settings);
}

/** The setting that would fill in the builder that was chosen, for the row that opens it. */
export function builderSettingOf(builder: Builder): string {
  return BUILDER[builder].setting;
}

/** Why a build cannot go ahead on a machine that has not got the builder it was told to use. */
export function missingBuilderOf(builder: Builder): string {
  return BUILDER[builder].missing;
}

/**
 * `DSSignFile.exe`, which signs whatever either builder produced. It comes with DayZ Tools, and
 * signing is its own step precisely so that it works the same way behind both builders.
 */
export function signToolOf(settings: MachineSettings): string {
  return settings.dayzTools === ''
    ? ''
    : windowsPath(settings.dayzTools, 'Bin', 'DsUtils', 'DSSignFile.exe');
}

export type EnvironmentKind = 'dayz' | 'dayzTools' | 'privateKey' | 'workDrive' | 'builder';

/** Set and there, set and gone, or never set: three states a developer acts on differently. */
export type EnvironmentState = 'ok' | 'missing' | 'unset';

/** One line of what the panel shows about the environment. */
export interface EnvironmentEntry {
  readonly kind: EnvironmentKind;
  /** The setting that fills it in. */
  readonly setting: string;
  readonly path: string;
  readonly state: EnvironmentState;
  /** True where being unset is a choice rather than a gap: an unsigned pbo is a pbo. */
  readonly optional: boolean;
}

/**
 * The paths the environment is made of, in the order the panel lists them. One table, so that the
 * paths that get checked for existence cannot drift from the paths that get reported.
 */
const ENTRIES: readonly {
  readonly kind: EnvironmentKind;
  /** The setting that fills it in, which for the builder is whichever one names the one chosen. */
  readonly setting: (settings: MachineSettings) => string;
  readonly of: (settings: MachineSettings) => string;
  readonly optional?: boolean;
}[] = [
  { kind: 'dayz', setting: () => SETTING.dayz, of: (settings) => settings.dayz },
  { kind: 'dayzTools', setting: () => SETTING.dayzTools, of: (settings) => settings.dayzTools },
  {
    kind: 'privateKey',
    setting: () => SETTING.privateKey,
    of: (settings) => settings.privateKey,
    optional: true,
  },
  { kind: 'workDrive', setting: () => SETTING.workDrive, of: (settings) => settings.workDrive },
  // The chosen builder rather than both of them: the one that is not going to pack anything is
  // not a gap, and saying it is missing would send a developer to install what they do not need.
  {
    kind: 'builder',
    setting: (settings) => builderSettingOf(settings.builder),
    of: builderExecutableOf,
  },
];

/** The paths worth asking the disk about, which is every one the environment is built from. */
export function environmentPaths(settings: MachineSettings): string[] {
  return ENTRIES.map((entry) => entry.of(settings)).filter((path) => path !== '');
}

/**
 * What resolved and what did not, from the settings and the paths that were found to exist. Which
 * of them exist is a fact about the disk, so it is handed in rather than looked up.
 */
export function environmentOf(
  settings: MachineSettings,
  present: readonly string[],
): EnvironmentEntry[] {
  const found = new Set(present.map(samePath));

  return ENTRIES.map((entry) => {
    const path = entry.of(settings);

    return {
      kind: entry.kind,
      setting: entry.setting(settings),
      path,
      state: stateOf(path, found),
      optional: entry.optional ?? false,
    };
  });
}

/** An unset private key is a choice — the pbo goes unsigned — and everything else is a gap. */
export function isWanting(entry: EnvironmentEntry): boolean {
  return entry.state === 'missing' || (entry.state === 'unset' && !entry.optional);
}

/** Anything the settings do not name is packed by pboProject, which is what most machines have. */
export function builderOf(value: string): Builder {
  return BUILDERS.find((builder) => builder === value) ?? 'pboProject';
}

function stateOf(path: string, found: ReadonlySet<string>): EnvironmentState {
  if (path === '') {
    return 'unset';
  }

  return found.has(samePath(path)) ? 'ok' : 'missing';
}
