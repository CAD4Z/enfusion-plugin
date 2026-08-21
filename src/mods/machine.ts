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

/** The program that packs an addon into a pbo. */
export type Builder = 'pboProject' | 'AddonBuilder';

export const BUILDERS: readonly Builder[] = ['pboProject', 'AddonBuilder'];

/** Everything the machine holds, already resolved: settings first, registry behind them. */
export interface MachineSettings {
  /** The DayZ installation — the folder the client and the diag executable sit in. */
  readonly dayz: string;
  readonly dayzTools: string;
  /** The `.biprivatekey` to sign with; empty means the pbo goes unsigned. */
  readonly privateKey: string;
  /** The folder the work drive is mounted from. */
  readonly workDrive: string;
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
  filePatchingRoot: 'enfusion.filePatching.root',
  builder: 'enfusion.builder',
} as const;

/** The section every one of them sits under. */
export const SECTION = 'enfusion';

export type EnvironmentKind = 'dayz' | 'dayzTools' | 'privateKey' | 'workDrive';

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
  readonly setting: string;
  readonly of: (settings: MachineSettings) => string;
  readonly optional?: boolean;
}[] = [
  { kind: 'dayz', setting: SETTING.dayz, of: (settings) => settings.dayz },
  { kind: 'dayzTools', setting: SETTING.dayzTools, of: (settings) => settings.dayzTools },
  {
    kind: 'privateKey',
    setting: SETTING.privateKey,
    of: (settings) => settings.privateKey,
    optional: true,
  },
  { kind: 'workDrive', setting: SETTING.workDrive, of: (settings) => settings.workDrive },
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
      setting: entry.setting,
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

/** Windows tells none of these apart, so neither does the comparison. */
function samePath(path: string): string {
  return path.replace(/\\/g, '/').replace(/\/+$/, '').toLowerCase();
}
