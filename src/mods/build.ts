/**
 * The build: what turns an addon on the work drive into a signed pbo in `<ModsDirectory>\@<Mod>`.
 *
 * An addon is what gets built, not a mod: a mod of several pbo is several builds, and the order
 * they go in is the one the model already worked out of the `requiredAddons` graph. The button is
 * unconditional — no staleness is tracked and nothing here asks whether a rebuild is needed —
 * because the one thing a build can honestly answer is whether a pbo is there afterwards.
 *
 * The whole of it comes out as a plan: an ordered list of steps, each carrying the command line
 * that will be run and the file that has to appear. Nothing here goes near a disk or a process,
 * which is what lets the plan be compared whole in a test instead of by watching what was called.
 *
 * Three things the previous implementation — the Workbench plugins in Enforce Script — was built
 * around are kept literally, and every one of them was re-checked against the real tools:
 *
 * - **The builder is started in a console of its own.** Without one pboProject exits immediately
 *   with code 1, writes nothing anywhere and packs nothing, which is impossible to tell apart from
 *   a real build failure. `start` is what gives it one.
 * - **Success is the pbo appearing, never the exit code.** Both builders answer 0 for a build that
 *   failed: pboProject writes `not produced due to error(s)` and exits 0, AddonBuilder writes
 *   `Build failed` and exits 0. So the old pbo comes off first, and what is there afterwards is
 *   this run's work.
 * - **A failed build is run once more, and then the log is named.** Which log that is differs per
 *   builder, and a step says which one it is rather than the runner guessing.
 *
 * One more was found while checking those: **pboProject silently does nothing when the folder it
 * is pointed at does not exist** — no console output, no log, exit 0, an empty output folder. So a
 * pack step names the folders to make before it, and they are made whether or not anything else
 * needs them.
 */

import {
  type Builder,
  type MachineSettings,
  builderExecutableOf,
  missingBuilderOf,
  signToolOf,
} from './machine';
import { CONFIG_FILE, type Mod } from './model';
import { resolveWindows, windowsFolder, windowsName, windowsPath } from './paths';
import type { Link } from './workDrive';

/** One addon to build, with everything about it that a plan reads. */
export interface BuildJob {
  /** Where the mod sits on the work drive, and whether it sits there at all. */
  readonly link: Link;
  /** The addon's folder name, which is also the name of the pbo it packs into. */
  readonly addon: string;
  /** Its path under the prefix root — `Scripts`, or empty in a single-addon mod. */
  readonly within: string;
  /** `modsDirectory` as the manifest wrote it; empty when nobody wrote one. */
  readonly modsDirectory: string;
  /** The folder a relative `modsDirectory` is counted from: the one holding the file that set it. */
  readonly configuredIn: string;
  /** The name of that file, for the sentence a missing `modsDirectory` is refused with. */
  readonly configuredBy: string;
  /** The masks the builder is not to pack, as the manifest wrote them. */
  readonly exclude: readonly string[];
}

/** A mod of the workspace as a build sees it: what the model made of it, and where it sits. */
export interface BuildSource {
  readonly mod: Mod;
  /** Its place on the work drive; undefined for a mod with no prefix root to put there. */
  readonly link: Link | undefined;
  /** Where the built mod goes, as the launch block that owns this mod wrote it. */
  readonly modsDirectory: string;
  /** What the mod's own manifest excludes from packing. */
  readonly exclude: readonly string[];
  /** The folder of the file the launch block came from, which a relative path is counted from. */
  readonly configuredIn: string;
  /** That file's name — `mod.enf` or `workspace.enf` — for the sentence that asks for a setting. */
  readonly configuredBy: string;
}

/**
 * Every addon of these mods, in the order they came in. Which is the order the model put them in,
 * and that order is the `requiredAddons` graph: mods after the mods they require, and the addons
 * inside each one after the addons they require. Nothing is re-sorted here — a build reads the
 * order off the model rather than working out one of its own.
 */
export function jobsOf(sources: readonly BuildSource[]): BuildJob[] {
  return sources.flatMap((source) => {
    const link = source.link;
    const prefixRoot = source.mod.prefixRoot;

    if (link === undefined || prefixRoot === undefined) {
      return [];
    }

    return source.mod.addons.map((addon) => ({
      link,
      addon: addon.name,
      within: withinOf(prefixRoot, addon.root),
      modsDirectory: source.modsDirectory,
      configuredIn: source.configuredIn,
      configuredBy: source.configuredBy,
      exclude: source.exclude,
    }));
  });
}

/** Everything a build will do, in the order it will do it, and what it will not do at all. */
export interface BuildPlan {
  readonly steps: readonly BuildStep[];
  readonly refusals: readonly BuildRefusal[];
  /**
   * What the developer asked for that this build will not honour, though it will go ahead anyway.
   * A thing quietly not done is worse than a thing refused out loud, and there is one of these:
   * AddonBuilder never sees an exclude list.
   */
  readonly warnings: readonly string[];
}

/** Why something is not going to be built. */
export interface BuildRefusal {
  /** What it is about, as `<Mod>\<Addon>`; empty where the reason stops every addon alike. */
  readonly subject: string;
  readonly reason: string;
}

export type BuildStep = PackStep | SignStep | CopyStep;

interface Step {
  /** What the step is about: an addon as `<Mod>\<Addon>`, or the mod itself for its root files. */
  readonly subject: string;
  /** What is being done, in the words the progress line and the log both use. */
  readonly what: string;
}

/** Running the builder over one addon, which is the whole of the build that can fail. */
export interface PackStep extends Step {
  readonly kind: 'pack';
  /**
   * Made first, and not only for tidiness: pboProject pointed at a folder that is not there packs
   * nothing at all and says nothing about it.
   */
  readonly folders: readonly string[];
  /**
   * Taken off first, so that a pbo found afterwards is this run's. A name may carry a `*`: a
   * signature is named after the key that made it, and one made by a key since replaced is stale
   * all the same — and a stale signature is what makes a server turn a freshly built mod away.
   */
  readonly stale: readonly string[];
  /** The command line, which puts the builder in a console of its own. */
  readonly command: string;
  /** The one thing success is judged by. */
  readonly pbo: string;
  /** Where the builder writes what it did, which is what a failure names and Problems reads. */
  readonly log: PackingLog;
  /** How many times the builder is run at most: once more after a run that produced nothing. */
  readonly attempts: number;
  /**
   * How long to leave between those runs. A pbo that could not be written because something still
   * had a handle on it is what the second run is for, and what the wait gives it time to let go.
   */
  readonly pauseMs: number;
}

/** A builder's log, and how it keeps it. */
export interface PackingLog {
  readonly path: string;
  /**
   * True where the builder appends every build of every addon to one file, so only what it added
   * this run belongs to this addon. pboProject writes one log per addon and rewrites it; DayZ
   * Tools keeps a single rolling `AddonBuilder.User.rpt` beside the tools themselves.
   */
  readonly appends: boolean;
}

/**
 * Signing a pbo, as its own step and through `DSSignFile.exe`, so that it works the same whichever
 * builder produced it — and so that a signature that could not be made is reported as that rather
 * than as a build failure. It is an ordinary console program, so unlike the builder it is run
 * directly, and a run of it costs no console window.
 */
export interface SignStep extends Step {
  readonly kind: 'sign';
  readonly program: string;
  readonly arguments: readonly string[];
}

/**
 * A file of the mod's root put into the built mod. Neither builder carries them: what is packed is
 * the addon, and `mod.cpp` sits above it. A source that is not there is not a failure — a mod
 * without a `mod.cpp` is a mod, it just does not name itself in the launcher.
 */
export interface CopyStep extends Step {
  readonly kind: 'copy';
  readonly from: string;
  readonly to: string;
}

/**
 * The masks a mod excludes when its manifest names none: the source formats a pbo has no business
 * carrying and the leavings of the tools. This is the list DayZ modding has settled on, and it is
 * a default rather than a floor — a manifest that names its own replaces it whole.
 *
 * `*.txt` is deliberately not here: pboProject packs a `.txt` whatever the exclude list says, and
 * a mask that is known to do nothing is worse than no mask at all.
 */
export const DEFAULT_EXCLUDE: readonly string[] = [
  '*.h',
  '*.hpp',
  '*.cpp',
  '*.png',
  '*.tga',
  '*.psd',
  '*.pew',
  '*.max',
  '*.fbx',
  '*.mcr',
  '*.dep',
  '*.bak',
  '*.log',
  '*.bat',
  '*.cmd',
  'thumbs.db',
  'source',
];

/** The folder of the built mod the pbo go into, which is the one `-mod=` is pointed at. */
const ADDONS_FOLDER = 'Addons';

/** And the one the public key goes into, which is where a server looks for it. */
const KEYS_FOLDER = 'Keys';

/** The root files a builder leaves behind, in the order they are copied. */
const ROOT_FILES: readonly string[] = ['mod.cpp', 'meta.cpp'];

/** A build that produced nothing is run once more before it is called a failure. */
const ATTEMPTS = 2;

/** Long enough for whatever was holding the pbo to have let go of it. */
const RETRY_PAUSE_MS = 3000;

/**
 * The plan: every step of building these addons, in this order, on this machine.
 *
 * A reason that stops every addon alike — no builder, a key that cannot be used — leaves no steps
 * at all, because running half a build to fail the same way on each addon helps nobody. A reason
 * that is one mod's own leaves that mod's addons out and builds the rest.
 */
export function buildPlanOf(jobs: readonly BuildJob[], settings: MachineSettings): BuildPlan {
  const stopping = stoppagesOf(settings);
  if (stopping.length > 0) {
    return {
      steps: [],
      refusals: stopping.map((reason) => ({ subject: '', reason })),
      warnings: [],
    };
  }

  const refusals: BuildRefusal[] = [];
  const building: BuildJob[] = [];

  for (const job of jobs) {
    const reason = refusalOf(job);
    if (reason === undefined) {
      building.push(job);
    } else {
      refusals.push({ subject: subjectOf(job), reason });
    }
  }

  const steps = building.flatMap((job, index) => [
    packOf(job, settings),
    ...(settings.privateKey === '' ? [] : [signOf(job, settings)]),
    // The root files belong to the mod rather than to one of its addons, so they are copied once,
    // after the last addon of it that is actually being built. Counting over the refused ones too
    // would hang them off an addon that produces no steps, and lose them.
    ...(isLastOfItsMod(building, index) ? copiesOf(job, settings) : []),
  ]);

  return { steps, refusals, warnings: warningsOf(building, settings) };
}

/** `<ModsDirectory>\@<Mod>`: the built mod, which is what is loaded rather than the sources. */
function builtModOf(job: BuildJob): string {
  return windowsPath(
    resolveWindows(job.configuredIn, job.modsDirectory),
    `@${job.link.name}`,
  );
}

/** The pbo this addon packs into: the one file the whole build is judged by. */
function pboOf(job: BuildJob): string {
  return windowsPath(builtModOf(job), ADDONS_FOLDER, `${job.addon}.pbo`);
}

/** `hurfy.bikey` next to `hurfy.biprivatekey`; empty when no key is set to sign with. */
function publicKeyOf(privateKey: string): string {
  if (privateKey === '') {
    return '';
  }

  const name = windowsName(privateKey);
  const stem = name.replace(/\.[^.]*$/, '');

  return windowsPath(windowsFolder(privateKey), `${stem}.bikey`);
}

/** What the addon is called on the work drive: `CADCore\Scripts`, or `CADCore` on its own. */
function prefixOf(job: BuildJob): string {
  return windowsPath(job.link.name, job.within);
}

/** And where that is: `P:\CADCore\Scripts`, which is the folder the builder is pointed at. */
function sourceOf(job: BuildJob): string {
  return windowsPath(job.link.path, job.within);
}

/**
 * The addon's `config.cpp` where the workspace keeps it, which is the file an addon is an addon
 * by. It is where a failure the builder gave no place for is pinned: a mark on the Problems list
 * belongs on the thing that would not build.
 */
export function configOf(job: BuildJob): string {
  return windowsPath(job.link.target, job.within, CONFIG_FILE);
}

/**
 * What is being asked for and will not happen. The build goes ahead regardless — a mod that packs
 * a few files it need not is still a mod that loads — but the developer wrote that list expecting
 * it to be honoured, and the one place they would find out otherwise is the documentation.
 */
function warningsOf(jobs: readonly BuildJob[], settings: MachineSettings): string[] {
  if (PACKING[settings.builder].excludes || !jobs.some((job) => job.exclude.length > 0)) {
    return [];
  }

  return [
    `${settings.builder} is packing the exclude list of ` +
      `${namesOf(jobs.filter((job) => job.exclude.length > 0))}: it takes one only through ` +
      '-exclude=, which crashes it. Build with pboProject to have the list honoured.',
  ];
}

function namesOf(jobs: readonly BuildJob[]): string {
  return [...new Set(jobs.map((job) => job.link.name))].join(', ');
}

/**
 * The reasons nothing can be built at all. Neither is about any one mod: a machine with no builder
 * on it builds nothing, and a key that is set but cannot be used would leave every pbo unsigned
 * without saying so — which is the one way a build can succeed and still be useless.
 */
function stoppagesOf(settings: MachineSettings): string[] {
  const stopping: string[] = [];

  if (builderExecutableOf(settings) === '') {
    stopping.push(missingBuilderOf(settings.builder));
  }

  if (settings.privateKey !== '' && signToolOf(settings) === '') {
    stopping.push(
      'A private key is set, but DSSignFile.exe was not found: set the DayZ Tools path, or clear ' +
        'the key to leave the pbo unsigned.',
    );
  }

  return stopping;
}


/**
 * Why this addon is not going to be built. Both reasons are the mod's own and both are visible on
 * the panel before the button is pressed — the point of saying them here is that the sentence a
 * refused build shows is the same one the panel was already showing.
 */
function refusalOf(job: BuildJob): string | undefined {
  const link = job.link;

  switch (link.state) {
    case 'unavailable':
      return `The work drive is not mounted, so there is nothing at ${link.path} to build.`;
    case 'unlinked':
      return `${link.name} is not on the work drive: nothing is at ${link.path}.`;
    case 'elsewhere':
      return `${link.path} points at ${link.at}, which is not ${link.name}.`;
    case 'occupied':
      return `${link.path} is not a link to ${link.name}, and what is there is not ours to move.`;
    case 'linked':
      return unconfiguredOf(job) ?? unquotableOf(job);
  }
}

function unconfiguredOf(job: BuildJob): string | undefined {
  return job.modsDirectory.trim() === ''
    ? `No mods directory is set: give ${job.configuredBy} a "launch" block with a ` +
        '"modsDirectory", which is where the built mod goes.'
    : undefined;
}

/**
 * A quotation mark in either of the two things the manifest puts on a command line. Windows allows
 * no such path and no such file mask, so nothing is lost by refusing them — and what would be
 * gained by letting them through is that a `mod.enf` from a repository somebody else wrote could
 * close a quote and start a command of its own.
 */
function unquotableOf(job: BuildJob): string | undefined {
  const value = [job.modsDirectory, ...job.exclude].find((text) => text.includes('"'));

  return value === undefined
    ? undefined
    : `${job.configuredBy} has a quotation mark in "${value}", which no path or mask can hold.`;
}

function packOf(job: BuildJob, settings: MachineSettings): PackStep {
  const built = builtModOf(job);
  const pbo = pboOf(job);

  return {
    kind: 'pack',
    subject: subjectOf(job),
    what: `Packing ${subjectOf(job)}`,
    folders: [built, windowsPath(built, ADDONS_FOLDER), windowsPath(built, KEYS_FOLDER)],
    stale: [pbo, `${pbo}.*.bisign`],
    command: commandOf(job, settings),
    pbo,
    log: PACKING[settings.builder].log(job, settings),
    attempts: ATTEMPTS,
    pauseMs: RETRY_PAUSE_MS,
  };
}

/**
 * The command line, and the `start` that wraps it. The title is given so that a quoted path to the
 * builder is not taken for one — `start` reads a leading quoted word as the window's title — and
 * `/wait` is what makes the shell hand back only once the builder is done.
 */
function commandOf(job: BuildJob, settings: MachineSettings): string {
  const builder = settings.builder;
  const program = builderExecutableOf(settings);
  const arguments_ = PACKING[builder].arguments(job).join(' ');

  return `start "${builder}" /wait ${quote(program)} ${arguments_}`;
}

/**
 * The whole of what the two builders differ by once one is chosen: how it is called, where it
 * writes about it, and whether the exclude list reaches it at all. One table, so that the answers
 * for one builder are read in one place and a third one is an entry rather than a hunt.
 */
const PACKING: Readonly<
  Record<
    Builder,
    {
      readonly arguments: (job: BuildJob) => string[];
      readonly log: (job: BuildJob, settings: MachineSettings) => PackingLog;
      /** False where the builder never sees the manifest's exclude list, and is told why. */
      readonly excludes: boolean;
    }
  >
> = {
  pboProject: {
    arguments: pboProjectArguments,
    // One log per addon, named after the folder it packed, in the `temp` of the work drive, and
    // rewritten every run.
    log: (job) => ({ path: windowsPath(tempOf(job), `${job.addon}.packing.log`), appends: false }),
    excludes: true,
  },
  AddonBuilder: {
    arguments: addonBuilderArguments,
    // One rolling file beside the tools for every build of every addon, appended to.
    log: (_job, settings) => ({
      path: windowsPath(settings.dayzTools, 'Bin', 'Logs', 'AddonBuilder.User.rpt'),
      appends: true,
    }),
    excludes: false,
  },
};

/**
 * `-P` batch mode, so it does not wait for a key press. `-R` leaves the settings of its own
 * window alone. `-W` keeps a warning from being an error. `-Key` tells it not to sign, because
 * signing is a step of its own. `-Mod=` is the built mod, and the pbo lands in `Addons` under it.
 */
function pboProjectArguments(job: BuildJob): string[] {
  const arguments_ = [
    '-P',
    '-R',
    '-W',
    '-Key',
    `-Mod=${quote(builtModOf(job))}`,
    quote(sourceOf(job)),
  ];

  const exclude = excludeOf(job);
  if (exclude.length > 0) {
    arguments_.push(`+X=${quote(exclude.join(','))}`);
  }

  return arguments_;
}

/**
 * The destination is the folder the pbo is written into rather than the built mod, which is where
 * AddonBuilder differs from pboProject. `-prefix` is what the addon is called on the work drive,
 * and `-project` is the root the config's includes are counted from — a drive letter and a
 * backslash, left unquoted because a quote after a backslash is an escaped quote to the program
 * reading it, and a drive root has no space in it to need quoting for.
 *
 * The exclude list is deliberately not passed. `-exclude=` crashes AddonBuilder 1.0.240639 with an
 * `ArgumentNullException` before it starts — with any list at all, including the `exclude.lst` its
 * own help points at — so passing one would mean every AddonBuilder build failing on a mod that
 * excludes anything. Which is the point of the default list, so: no list, and a builder that works.
 */
function addonBuilderArguments(job: BuildJob): string[] {
  return [
    quote(sourceOf(job)),
    quote(windowsPath(builtModOf(job), ADDONS_FOLDER)),
    `-prefix=${quote(prefixOf(job))}`,
    `-project=${driveRootOf(job)}`,
    `-temp=${quote(tempOf(job))}`,
  ];
}

function signOf(job: BuildJob, settings: MachineSettings): SignStep {
  return {
    kind: 'sign',
    subject: subjectOf(job),
    what: `Signing ${job.addon}.pbo`,
    program: signToolOf(settings),
    arguments: [settings.privateKey, pboOf(job)],
  };
}

/** `mod.cpp`, `meta.cpp` and the public key: the built mod's own files, none of them packed. */
function copiesOf(job: BuildJob, settings: MachineSettings): CopyStep[] {
  const built = builtModOf(job);
  const mod = job.link.name;

  const roots = ROOT_FILES.map((file) => ({
    kind: 'copy' as const,
    subject: mod,
    what: `Copying ${file}`,
    from: windowsPath(job.link.target, file),
    to: windowsPath(built, file),
  }));

  const key = publicKeyOf(settings.privateKey);
  if (key === '') {
    return roots;
  }

  return [
    ...roots,
    {
      kind: 'copy',
      subject: mod,
      what: `Copying ${windowsName(key)}`,
      from: key,
      to: windowsPath(built, KEYS_FOLDER, windowsName(key)),
    },
  ];
}

/** What the manifest named, or the list a mod gets for naming nothing. */
function excludeOf(job: BuildJob): readonly string[] {
  return job.exclude.length > 0 ? job.exclude : DEFAULT_EXCLUDE;
}

/** The folder both builders keep their workings in, which is `temp` on the work drive. */
function tempOf(job: BuildJob): string {
  return windowsPath(driveOf(job), 'temp');
}

/** `P:`, out of the link the mod sits behind. */
function driveOf(job: BuildJob): string {
  return job.link.path.split(/[\\/]/)[0] ?? '';
}

/** `P:\`, which is what a program counting paths from the root of the drive is given. */
function driveRootOf(job: BuildJob): string {
  return `${driveOf(job)}\\`;
}

export function subjectOf(job: BuildJob): string {
  return windowsPath(job.link.name, job.within === '' ? '' : job.addon);
}

/**
 * The addon's path under the prefix root, the way a command line takes it: empty where the addon
 * is the prefix root itself, which is the whole of a single-addon mod.
 */
function withinOf(prefixRoot: string, addonRoot: string): string {
  return addonRoot === prefixRoot
    ? ''
    : addonRoot.slice(prefixRoot.length + 1).replace(/\//g, '\\');
}

/** The last of its mod, which is the addon the mod's root files are copied after. */
function isLastOfItsMod(jobs: readonly BuildJob[], index: number): boolean {
  const mod = jobs[index]?.link.name;

  return !jobs.slice(index + 1).some((later) => later.link.name === mod);
}

/** Quoted whatever it holds: a path with no space in it today is one folder away from having one. */
function quote(value: string): string {
  return `"${value}"`;
}
