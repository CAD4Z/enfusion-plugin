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
 * What the settings and the registry between them could not answer goes to the log at every scan,
 * and is what the button that needs it refuses over.
 */

import { resolveWindows, samePath, windowsFolder, windowsName, windowsPath } from './paths';

/** The program that packs an addon into a pbo. */
export type Builder = 'pboProject' | 'AddonBuilder';

export const BUILDERS: readonly Builder[] = ['pboProject', 'AddonBuilder'];

/**
 * Which build of the game a launch starts.
 *
 * `Debug` is the diag build, which is one executable playing both parts and the only one that
 * honours `-filePatching` or carries a script debugger. `Release` is what a player's machine runs:
 * two programs out of two installations, loading the pbo that were actually packed. Debug is what
 * a mod is written under; Release is what says whether what was packed is the same mod.
 */
export type GameBuild = 'Debug' | 'Release';

export const GAME_BUILDS: readonly GameBuild[] = ['Debug', 'Release'];

/** Anything else — an older remembered choice, a hand-edited configuration — is the diag build. */
export function gameBuildOf(value: unknown): GameBuild {
  return GAME_BUILDS.find((build) => build === value) ?? 'Debug';
}

/** Everything the machine holds, already resolved: settings first, registry behind them. */
export interface MachineSettings {
  /** The DayZ installation — the folder the client and the diag executable sit in. */
  readonly dayz: string;
  /** The separately installed DayZ Experimental client and diag build. */
  readonly dayzExperimental: string;
  /** The diag executable, as a name in that folder or as a path of its own. */
  readonly executable: string;
  /** The DayZ Server installation; empty is the folder Steam puts it in beside DayZ. */
  readonly dayzServer: string;
  /** The separately installed DayZ Experimental Server application. */
  readonly dayzExperimentalServer: string;
  readonly dayzTools: string;
  /** `pboProject.exe`, or the folder holding it; see `pboProjectExecutableOf`. */
  readonly pboProject: string;
  /** Whether a build signs what it packed. On, and turned off by a developer who has no use for it. */
  readonly signing: boolean;
  /** The `.biprivatekey` to sign with; empty means the pbo goes unsigned. See `signingKeyOf`. */
  readonly privateKey: string;
  /** The folder the work drive is mounted from. */
  readonly workDrive: string;
  /** The letter it is mounted under, however it was typed; see `driveLetterOf`. */
  readonly workDriveLetter: string;
  /** Where the file patching root is built; empty leaves the extension to pick the place. */
  readonly filePatchingRoot: string;
  /** Where the profiles are built; empty puts them in the run folder. See `profilesRootOf`. */
  readonly profiles: string;
  /**
   * What the second client needs to be a second client: which Steam account it signs in as, and
   * the two installations that let it.
   *
   * On one machine two clients are two Steam accounts, and one signed-in account per Windows
   * session is all Steam allows — so the second is run inside a Sandboxie box with a Steam of its
   * own. See `src/mods/sandbox.ts`, which is where all of that is worked out.
   */
  readonly secondClient: SecondClient;
  readonly builder: Builder;
}

/**
 * Who the second client signs in as, and where the programs that let it are.
 *
 * An empty `account` is the ordinary case on a machine not set up for two: the second client is
 * then simply another client, which is worth having for offline work and cannot join a server the
 * first one is already signed in to. It is also the only one of the three a developer types —
 * Sandboxie and Steam both record where they are, and are read from there.
 */
export interface SecondClient {
  /** The Steam account the second client signs in as; empty is a machine with only one. */
  readonly account: string;
  /** Sandboxie-Plus's folder — the one holding `Start.exe` and `SbieIni.exe`. */
  readonly sandboxie: string;
  /** Steam's folder — the one holding `steam.exe`. */
  readonly steam: string;
}

/** The settings, by the ids the editor knows them under, so the panel can open the right one. */
export const SETTING = {
  dayz: 'enfusion.dayz.path',
  dayzExperimental: 'enfusion.dayzExperimental.path',
  executable: 'enfusion.dayz.executable',
  dayzServer: 'enfusion.dayzServer.path',
  dayzExperimentalServer: 'enfusion.dayzExperimentalServer.path',
  dayzTools: 'enfusion.dayzTools.path',
  signing: 'enfusion.signing.enabled',
  privateKey: 'enfusion.signing.privateKey',
  workDrive: 'enfusion.workDrive.source',
  workDriveLetter: 'enfusion.workDrive.letter',
  filePatchingRoot: 'enfusion.filePatching.root',
  profiles: 'enfusion.launch.profiles',
  secondAccount: 'enfusion.launch.secondAccount',
  sandboxie: 'enfusion.sandboxie.path',
  steam: 'enfusion.steam.path',
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
  // pboProject's installer records the executable itself, so the setting names a file — and takes
  // the folder holding it too, which is what a developer asked "where is pboProject" reaches for.
  pboProject: {
    executable: (settings) => pboProjectExecutableOf(settings.pboProject),
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

/** What Mikero's installer calls its own program, and what the setting is completed to. */
const PBOPROJECT_EXE = 'pboProject.exe';

/**
 * `pboProject.exe` out of whatever the setting holds.
 *
 * The setting asks for the executable, because that is what Mikero's installer records. The same
 * registry key records the folder holding it as well, and the folder is what a developer reaches
 * for when asked where a program is — so a path that does not name an executable is taken as the
 * folder it sits in, and completed.
 *
 * Worth the trouble because of how the mistake fails. The builder is run through `start`, and
 * `start` hands anything it cannot execute to the shell: a folder there opens in Explorer, nothing
 * is packed, and the build fails pointing at a packing log that some earlier run wrote.
 */
export function pboProjectExecutableOf(path: string): string {
  if (path === '') {
    return '';
  }

  // Any executable is taken as the program: a copy under another name is still the program.
  return /\.exe$/i.test(windowsName(path)) ? path : windowsPath(path, PBOPROJECT_EXE);
}

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

/**
 * DayZ's work-drive helper. Unlike one `subst` call, it creates the P: mapping in both the normal
 * and elevated Windows token contexts, so Workbench sees the same drive however it was started.
 */
export function workDriveToolOf(settings: MachineSettings): string {
  return settings.dayzTools === ''
    ? ''
    : windowsPath(settings.dayzTools, 'Bin', 'WorkDrive', 'WorkDrive.exe');
}

/** DayZ Workbench, installed as part of DayZ Tools. */
export function workbenchExecutableOf(settings: MachineSettings): string {
  return settings.dayzTools === ''
    ? ''
    : windowsPath(settings.dayzTools, 'Bin', 'Workbench', 'workbenchApp.exe');
}

/**
 * The key this machine signs with, and empty where nothing is going to be signed at all — signing
 * turned off, or left on with no key named.
 *
 * One answer, because every part of a build that turns on whether there will be a signature turns
 * on the same thing: the step that makes it, the public key copied in beside it for a server to
 * check it against, and the refusal over a `DSSignFile.exe` that is not there. Two of those
 * agreeing and the third not is a mod that ships a key it was never signed with, or one that
 * refuses to build over a signature nobody asked for.
 */
export function signingKeyOf(settings: MachineSettings): string {
  return settings.signing ? settings.privateKey : '';
}

/** The build of the game that reads scripts off the disk rather than out of a pbo. */
export const DEFAULT_EXECUTABLE = 'DayZDiag_x64.exe';

/** The retail pair: the client a player starts, and the server a host does. */
const RELEASE_CLIENT = 'DayZ_x64.exe';
const RELEASE_SERVER = 'DayZServer_x64.exe';

/** What Steam calls the DayZ Server folder, which it installs beside DayZ rather than inside it. */
const SERVER_FOLDER = 'DayZServer';

/** Steam's default install folder for the experimental server. */
const EXPERIMENTAL_SERVER_FOLDER = 'DayZ Server Exp';

/** Which role's part of a launch is being started, as far as choosing a program goes. */
export type GameSide = 'client' | 'server';

/** A program to start, out of the installation it belongs to. */
export interface GameProgram {
  /**
   * The installation it comes out of. A release build is started in it — nothing is patched into
   * a retail game, so there is no mirror for it to run in and no reason to build one.
   */
  readonly root: string;
  /** What it is called, which a machine with no installation set still knows. */
  readonly name: string;
  /** Where it is; empty where nothing said which installation it is in. */
  readonly path: string;
}

/**
 * The program each side of a launch starts, which is a fact about the build and about the role.
 *
 * The diag build is one executable and one installation: told `-server` it is the server, and it
 * is the only build that honours `-filePatching`, which is why it is what `Debug` means. The
 * retail build is two — `DayZ_x64.exe` out of DayZ, `DayZServer_x64.exe` out of DayZ Server —
 * because Steam sells and installs them as two things.
 *
 * `enfusion.dayz.executable` names the stable diag build only. It is there for a developer whose
 * diag build sits outside the installation. An Experimental target takes the shipped diag name
 * from its own installation so an absolute stable override cannot defeat the target; a retail
 * launch has nothing to override either.
 */
export function gameProgramOf(
  settings: MachineSettings,
  build: GameBuild,
  side: GameSide,
  experimental = false,
): GameProgram {
  const clientRoot = experimental ? settings.dayzExperimental : settings.dayz;

  if (build === 'Debug') {
    // The override belongs to the ordinary development installation. An Experimental target is
    // specifically a request for the executable Steam installed into DayZ Exp, so an absolute
    // stable/custom diag path must not quietly win over that target choice.
    const executable =
      settings.executable === '' || experimental ? DEFAULT_EXECUTABLE : settings.executable;

    return {
      root: clientRoot,
      name: windowsName(executable),
      path: resolveWindows(clientRoot, executable),
    };
  }

  if (side === 'server') {
    const root = dayzServerRootOf(settings, experimental);

    return {
      root,
      name: RELEASE_SERVER,
      path: root === '' ? '' : windowsPath(root, RELEASE_SERVER),
    };
  }

  return {
    root: clientRoot,
    name: RELEASE_CLIENT,
    path: clientRoot === '' ? '' : windowsPath(clientRoot, RELEASE_CLIENT),
  };
}

/**
 * Where DayZ Server is. Steam installs it as an application of its own, beside DayZ in the same
 * library and never inside it, and its installer writes no registry key of the kind the client's
 * does — so with nothing set the folder beside DayZ is the guess, and it is a good one on a
 * machine where both came from Steam. A setting overrides it for a machine where they did not.
 */
export function dayzServerRootOf(settings: MachineSettings, experimental = false): string {
  const configured = experimental ? settings.dayzExperimentalServer : settings.dayzServer;
  if (configured !== '') {
    return configured;
  }

  const library = windowsFolder(experimental ? settings.dayzExperimental : settings.dayz);
  const folder = experimental ? EXPERIMENTAL_SERVER_FOLDER : SERVER_FOLDER;

  return library === '' ? '' : windowsPath(library, folder);
}

/** Why a release launch cannot start that side of itself, in the words that say what to do. */
export function missingProgramOf(
  build: GameBuild,
  side: GameSide,
  program: string,
  experimental = false,
): string {
  if (build === 'Debug') {
    if (!experimental) {
      return (
        `${program} is not there. File patching needs the diag build of the game, which comes ` +
        `with DayZ Tools; ${SETTING.executable} names the one to start.`
      );
    }

    return (
      `${program} is not there. File patching needs the diag build of DayZ Experimental; ` +
      `install it, or name its folder with ${SETTING.dayzExperimental}.`
    );
  }

  if (!experimental) {
    return side === 'server'
      ? `${program} is not there. DayZ Server is a Steam application of its own — install it, or ` +
          `name the folder it is in with ${SETTING.dayzServer}.`
      : `${program} is not there. A Release launch starts the game a player starts, out of the ` +
          `installation ${SETTING.dayz} names.`;
  }

  return side === 'server'
    ? `${program} is not there. DayZ Experimental Server is a Steam application of its own — ` +
        `install it, or name the folder it is in with ` +
        `${SETTING.dayzExperimentalServer}.`
    : `${program} is not there. A Release launch starts the game a player starts, out of the ` +
        `installation ${SETTING.dayzExperimental} names.`;
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
  /**
   * True where being unset is a choice rather than a gap. Only the private key is ever that, and
   * only with signing turned off: an unsigned pbo somebody asked for is a pbo, and an unsigned pbo
   * out of a build that says it signs is a mod no server will take.
   */
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
  /** Whether leaving this one empty is a choice, which for the key depends on the signing flag. */
  readonly optional?: (settings: MachineSettings) => boolean;
}[] = [
  { kind: 'dayz', setting: () => SETTING.dayz, of: (settings) => settings.dayz },
  { kind: 'dayzTools', setting: () => SETTING.dayzTools, of: (settings) => settings.dayzTools },
  // A gap while signing is on, and a choice once it is off, which is the same fact read twice: the
  // key is wanted exactly as much as the signature it would make.
  {
    kind: 'privateKey',
    setting: () => SETTING.privateKey,
    of: (settings) => settings.privateKey,
    optional: (settings) => !settings.signing,
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
      optional: entry.optional?.(settings) ?? false,
    };
  });
}

/** A gap wants attention; a thing left unset on purpose does not. */
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
