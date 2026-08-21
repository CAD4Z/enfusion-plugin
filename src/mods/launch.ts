/**
 * The launch: what turns a target out of a `.enf` into a running game.
 *
 * `-filePatching` makes the engine look for an addon's files by its prefix **relative to the
 * working directory of the process**, so somebody has to put links to the sources there. Workbench
 * does it inside its own folder and the game inherits that working directory; we start the game
 * ourselves, so we choose the folder ourselves, and we build it in
 * `%LOCALAPPDATA%\Enfusion\run\<workspace>`: junctions onto everything the game's root holds, plus
 * junctions onto the prefix roots of the mods. Neither the game folder nor the work drive is
 * touched to launch. See `docs/adr/0001-own-file-patching-root.md`.
 *
 * The game's root is mirrored **by listing it**, never off a list of names known in advance: the
 * engine's folders drift between versions, and the Workbench plugin's hardcoded `Addons`, `bliss`,
 * `sakhal` has had `bliss` hanging as a broken link for a good while now.
 *
 * As with a build, the whole of it comes out as a plan — folders, links, command lines — and
 * nothing here goes near a disk or a process. What the disk holds (what is in the game's root,
 * what is in the run folder already, whether the executable is there) is handed in as plain data,
 * which is what lets the plan be compared whole in a test.
 */

import { sameName } from './config';
import type { Launch, Run, Target } from './enf';
import type { MachineSettings } from './machine';
import { resolveWindows, samePath, windowsPath } from './paths';
import type { LinkFact, WorkDrive } from './workDrive';

/** Where the run folders go when the settings name no place of their own. */
const RUN_FOLDER: readonly string[] = ['Enfusion', 'run'];

/**
 * The folder the game is launched in: one per workspace, so that two projects open side by side do
 * not fight over one set of links. The name is the workspace's own, which is what makes the folder
 * recognisable to whoever finds it under `%LOCALAPPDATA%` a year from now.
 */
export function runRootOf(configured: string, localAppData: string, workspace: string): string {
  const base =
    configured.trim() === '' ? windowsPath(localAppData, ...RUN_FOLDER) : configured.trim();

  return windowsPath(base, folderNameOf(workspace));
}

/** A workspace is named by whoever opened it, and a name is not a folder name until it can be one. */
function folderNameOf(workspace: string): string {
  const name = workspace.replace(/[<>:"/\\|?*]+/g, '-').replace(/^[.\s]+|[.\s]+$/g, '');

  return name === '' ? 'workspace' : name;
}

/** A mod's launch block, with the file it came from: the cascade has already picked which one. */
export interface TargetSource {
  /** The mod this block configures, by the name it is linked and loaded under. */
  readonly mod: string;
  /** The `.enf` the block was read from; empty for a mod that has no manifest at all. */
  readonly owner: string;
  /** That file's name — `mod.enf` or `workspace.enf` — for the sentence that asks for a setting. */
  readonly configuredBy: string;
  /** Its folder, which is what a relative path in it is counted from. */
  readonly configuredIn: string;
  readonly launch: Launch;
}

/** One entry of the Run and Debug list, with everything a launch reads about it. */
export interface LaunchTarget {
  /** What a debug configuration names it by: the target's name, qualified only where it must be. */
  readonly id: string;
  readonly name: string;
  /** The mod the profile and the mission come from, and the one the target is listed under. */
  readonly mod: string;
  readonly map: string | undefined;
  readonly run: Run;
  readonly serverConfig: string | undefined;
  /** The block the target was declared in: where the mods directory and the mod lists come from. */
  readonly launch: Launch;
  readonly configuredIn: string;
  readonly configuredBy: string;
}

/**
 * The targets of a workspace, in the order the mods came in.
 *
 * A `workspace.enf` owns the launch of every mod under it, so its targets would otherwise be
 * counted once per mod: a block is read from the file that holds it, once, however many mods obey
 * it. Which is also why a target of that block belongs to the first mod obeying it rather than to
 * all of them — and in build order the first mod is the one the others are built on.
 */
export function targetsOf(sources: readonly TargetSource[]): LaunchTarget[] {
  const seen = new Set<string>();
  const drafts: LaunchTarget[] = [];

  for (const source of sources) {
    const owner = samePath(source.owner);
    if (owner === '' || seen.has(owner)) {
      continue;
    }

    seen.add(owner);
    drafts.push(...source.launch.targets.map((target) => draftOf(target, source)));
  }

  return drafts.map((draft, index) => ({ ...draft, id: idOf(draft, drafts, index) }));
}

/** The target a debug configuration asked for: by the id it was offered under, or by its name. */
export function targetById(
  targets: readonly LaunchTarget[],
  id: string,
): LaunchTarget | undefined {
  return targets.find((target) => target.id === id) ?? targets.find((target) => target.name === id);
}

function draftOf(target: Target, source: TargetSource): LaunchTarget {
  return {
    id: target.name,
    name: target.name,
    mod: target.mod ?? source.mod,
    map: target.map,
    run: target.run,
    serverConfig: target.serverConfig,
    launch: source.launch,
    configuredIn: source.configuredIn,
    configuredBy: source.configuredBy,
  };
}

/**
 * A name is what a developer types into a debug configuration, so it has to mean one target. Two
 * mods of a monorepo each calling a target "Client" is ordinary, and both keep their name with the
 * mod's in front of it rather than one of them silently winning the name.
 */
function idOf(target: LaunchTarget, all: readonly LaunchTarget[], index: number): string {
  const shared = all.some((other, at) => at !== index && other.name === target.name);

  return shared ? `${target.mod}: ${target.name}` : target.name;
}

/** One entry of the game's root, as a listing of it answered. */
export interface GameEntry {
  readonly name: string;
  readonly directory: boolean;
}

/** A mod as a launch sees it: the name it is linked under, and the folder that goes there. */
export interface LaunchMod {
  readonly name: string;
  /** The prefix root the way Windows takes it, which is what the run folder links to. */
  readonly prefixRoot: string;
}

/** A junction to make: the link, and what it points at. */
export interface Junction {
  readonly path: string;
  readonly target: string;
}

export interface FileCopy {
  readonly from: string;
  readonly to: string;
}

/** What the run folder has to be made into before the game is started in it. */
export interface FilePatchingPlan {
  readonly root: string;
  /** The links to make, the game's folders first and the mods after them. */
  readonly junctions: readonly Junction[];
  /** Links to take off first: ones pointing elsewhere, and ones nobody asks for any more. */
  readonly remove: readonly string[];
  readonly copies: readonly FileCopy[];
  /** Paths where something that is not a link of ours sits, so the link cannot be made. */
  readonly conflicts: readonly string[];
}

export interface FilePatchingInput {
  readonly root: string;
  /** The game's installation, whose root is what gets mirrored. */
  readonly game: string;
  readonly entries: readonly GameEntry[];
  readonly mods: readonly LaunchMod[];
  /** What is in the run folder now, by the name it goes by there. */
  readonly present: ReadonlyMap<string, LinkFact>;
}

/**
 * The files the game reads out of its working directory rather than out of its own folder. Unlike
 * the folders this is a list rather than a listing, and it can be: what is on it is the handful of
 * small files that have to sit beside the process, while the rest of the root is executables and
 * their libraries — which the game finds through the path it was started by, and which copying
 * would mean copying hundreds of megabytes on every launch.
 */
const CARRIED_FILES: readonly string[] = ['steam_appid.txt'];

/**
 * The run folder as it should be, against what is in it now.
 *
 * Every folder of the game's root is linked — by listing that root, so that a world added in a
 * patch is mirrored the day it appears — and so is every mod's prefix root. A link already
 * pointing where it should is left alone rather than remade; one pointing elsewhere is taken off
 * and made again; and a link nobody asks for any more is taken off, which is what keeps a folder
 * dropped from the game and a mod dropped from the workspace from hanging there broken.
 *
 * What is not a link is not ours: the game writes into its working directory, and a file it left
 * there is neither removed nor reported. The exception is a name that is needed — something real
 * sitting where a junction has to go — and that comes back as a conflict rather than being deleted.
 */
export function filePatchingPlanOf(input: FilePatchingInput): FilePatchingPlan {
  const wanted = wantedOf(input);
  const facts = new Map(
    [...input.present].map(([name, fact]) => [name.toLowerCase(), { name, fact }] as const),
  );

  const junctions: Junction[] = [];
  const remove: string[] = [];
  const conflicts: string[] = [];

  for (const [key, junction] of wanted) {
    const fact: LinkFact = facts.get(key)?.fact ?? { kind: 'none' };

    if (fact.kind === 'occupied') {
      conflicts.push(junction.path);
      continue;
    }

    if (fact.kind === 'link') {
      if (samePath(fact.target) === samePath(junction.target)) {
        continue;
      }

      remove.push(junction.path);
    }

    junctions.push(junction);
  }

  for (const [key, { name, fact }] of facts) {
    if (fact.kind === 'link' && !wanted.has(key)) {
      remove.push(windowsPath(input.root, name));
    }
  }

  return { root: input.root, junctions, remove, copies: copiesOf(input), conflicts };
}

/**
 * Every link the run folder should hold, keyed by the name it goes by. The mods come after the
 * game's folders and under that same key: a mod named after one of them takes the name, because a
 * mod called `Addons` is a mod that has already decided what it means that name to point at.
 */
function wantedOf(input: FilePatchingInput): Map<string, Junction> {
  const wanted = new Map<string, Junction>();

  for (const entry of input.entries.filter((entry) => entry.directory)) {
    wanted.set(entry.name.toLowerCase(), {
      path: windowsPath(input.root, entry.name),
      target: windowsPath(input.game, entry.name),
    });
  }

  for (const mod of input.mods) {
    wanted.set(mod.name.toLowerCase(), {
      path: windowsPath(input.root, mod.name),
      target: mod.prefixRoot,
    });
  }

  return wanted;
}

function copiesOf(input: FilePatchingInput): FileCopy[] {
  return input.entries
    .filter((entry) => !entry.directory && CARRIED_FILES.some((file) => sameName(file, entry.name)))
    .map((entry) => ({
      from: windowsPath(input.game, entry.name),
      to: windowsPath(input.root, entry.name),
    }));
}

/** The game's installation as a launch reads it: where it is, what starts it, what it holds. */
export interface GameRoot {
  readonly path: string;
  readonly executable: string;
  /** Whether that executable is actually there, which is a fact about the disk. */
  readonly present: boolean;
  readonly entries: readonly GameEntry[];
}

/** Everything a launch is planned from. */
export interface LaunchInput {
  readonly target: LaunchTarget;
  /** The mods of the workspace, in the order the model put them in. */
  readonly mods: readonly LaunchMod[];
  readonly settings: MachineSettings;
  readonly drive: WorkDrive;
  readonly runRoot: string;
  readonly game: GameRoot;
  /** What the run folder holds now, by name. */
  readonly present: ReadonlyMap<string, LinkFact>;
}

export type LaunchRole = 'client' | 'server';

/** One process to start, and the working directory that makes file patching find the sources. */
export interface LaunchProcess {
  readonly role: LaunchRole;
  /** What is being done, in the words the progress line and the log both use. */
  readonly what: string;
  readonly program: string;
  readonly arguments: readonly string[];
  readonly cwd: string;
}

/** Everything a launch will do, and what it will not do at all. */
export interface LaunchPlan {
  /** Why nothing is going to start. Any one of them leaves the plan with nothing in it. */
  readonly refusals: readonly string[];
  /** What was asked for that this launch will not honour, though it goes ahead anyway. */
  readonly warnings: readonly string[];
  readonly filePatching: FilePatchingPlan;
  /** Folders to make: the run folder itself, and the profile the game writes its logs into. */
  readonly folders: readonly string[];
  readonly processes: readonly LaunchProcess[];
}

/**
 * The arguments the client is started with. The diag build's logging is half the point of
 * launching from here — a script error belongs in a log the developer reads rather than in a
 * message the game swallows — and `-window` is what makes it possible to alt-tab back to the
 * editor that started it.
 */
const CLIENT_ARGUMENTS: readonly string[] = [
  '-filePatching',
  '-scriptDebug=true',
  '-newErrorsAreWarnings=1',
  '-doLogs',
  '-adminlog',
  '-nopause',
  '-nosplash',
  '-window',
];

/** The folder the game writes its logs, its `.RPT` and the player's own settings into. */
const PROFILES_FOLDER = 'profiles';

/**
 * The plan: the run folder made ready, and the game started in it.
 *
 * A refusal leaves no steps at all — half a launch is a game that comes up without the mod it was
 * launched for, which is the failure this exists to prevent rather than to cause.
 */
export function launchPlanOf(input: LaunchInput): LaunchPlan {
  const refusals = refusalsOf(input);
  if (refusals.length > 0) {
    return {
      refusals,
      warnings: [],
      filePatching: nothing(input.runRoot),
      folders: [],
      processes: [],
    };
  }

  const filePatching = filePatchingPlanOf({
    root: input.runRoot,
    game: input.game.path,
    entries: input.game.entries,
    mods: input.mods,
    present: input.present,
  });
  const profile = profileOf(input, 'client');

  return {
    refusals: [],
    warnings: warningsOf(input, filePatching),
    filePatching,
    folders: [input.runRoot, profile],
    processes: [clientOf(input, profile)],
  };
}

function nothing(root: string): FilePatchingPlan {
  return { root, junctions: [], remove: [], copies: [], conflicts: [] };
}

/**
 * Why nothing is going to start. Every one of them is something a developer can put right, and
 * every one is said before a process is spawned rather than after the game has come up without its
 * mod — which is the failure that costs an hour, because it looks like a bug in the mod.
 */
function refusalsOf(input: LaunchInput): string[] {
  const said: string[] = [
    ...gameRefusalOf(input),
    ...driveRefusalOf(input.drive),
  ];

  if (input.target.run === 'server') {
    said.push(
      `${input.target.name} puts up the server alone, and starting the server is not implemented yet.`,
    );
  }

  if (modOf(input) === undefined) {
    said.push(
      `${input.target.name} launches ${input.target.mod}, which is not a mod of this workspace.`,
    );
  }

  if (modsDirectoryOf(input.target) === '') {
    said.push(
      `No mods directory is set: give ${input.target.configuredBy} a "launch" block with a ` +
        '"modsDirectory", which is where the built mods are loaded from.',
    );
  }

  said.push(...unquotableOf(input.target));

  return said;
}

function gameRefusalOf(input: LaunchInput): string[] {
  if (input.settings.dayz === '' && input.settings.executable === '') {
    return [
      'No DayZ installation is set: fill in enfusion.dayz.path, which is otherwise read from the ' +
        'registry its installer wrote it to.',
    ];
  }

  if (!input.game.present) {
    return [
      `${input.game.executable} is not there. File patching needs the diag build of the game, ` +
        'which comes with DayZ Tools; enfusion.dayz.executable names the one to start.',
    ];
  }

  return [];
}

/**
 * A launch off a drive that is not up is a launch off sources nothing was built from: a mod is
 * linked onto the work drive, packed from it, and patched out of it.
 */
function driveRefusalOf(drive: WorkDrive): string[] {
  switch (drive.state) {
    case 'unset':
      return [
        'No folder is set to mount the work drive from, so the mods have no sources to patch from.',
      ];
    case 'unmounted':
      return [`${drive.letter} is not mounted, so the mods have no sources to patch from.`];
    case 'mounted':
    case 'elsewhere':
      return [];
  }
}

/**
 * A quotation mark in what the manifest puts on a command line. No Windows path holds one, so
 * nothing is lost by refusing it — and an argument that ends where nobody meant it to is worth
 * refusing before it reaches a process rather than after.
 */
function unquotableOf(target: LaunchTarget): string[] {
  const value = [
    target.launch.modsDirectory ?? '',
    ...target.launch.clientMods,
    ...target.launch.serverMods,
  ].find((text) => text.includes('"'));

  return value === undefined
    ? []
    : [`${target.configuredBy} has a quotation mark in "${value}", which no path can hold.`];
}

/** What was asked for and will not happen, though the launch goes ahead regardless. */
function warningsOf(input: LaunchInput, filePatching: FilePatchingPlan): string[] {
  const said: string[] = [];

  if (input.target.run === 'both') {
    said.push(
      `${input.target.name} asks for a server as well, and only the client is started: ` +
        'starting the server is not implemented yet.',
    );
  }

  for (const path of filePatching.conflicts) {
    said.push(`${path} is not a link of ours, so it is left as it is and nothing is patched into it.`);
  }

  return said;
}

function clientOf(input: LaunchInput, profile: string): LaunchProcess {
  const mods = modListOf(input);

  return {
    role: 'client',
    what: `Starting the client for ${input.target.name}`,
    program: input.game.executable,
    arguments: [
      ...CLIENT_ARGUMENTS,
      `-profiles=${profile}`,
      `-mod=${mods}`,
      ...missionOf(input),
    ],
    cwd: input.runRoot,
  };
}

/**
 * A client with no server to join loads an offline mission of the target's world, which is what
 * makes a target of one map worth having at all. With no map it comes up at the main menu.
 */
function missionOf(input: LaunchInput): string[] {
  const map = input.target.map;

  return input.target.run === 'client' && map !== undefined && map !== ''
    ? [`-mission=dayzOffline.${map}`]
    : [];
}

/**
 * The `-mod=` list: the third-party mods first and the workspace's own after them, which is load
 * order — a mod is loaded after what it is built on. The workspace's own are already in that
 * order, because the model read it off the `requiredAddons` graph.
 */
function modListOf(input: LaunchInput): string {
  const directory = modsDirectoryOf(input.target);
  const third = input.target.launch.clientMods.map((name) => windowsPath(directory, atOf(name)));
  const own = input.mods.map((mod) => windowsPath(directory, atOf(mod.name)));

  return [...third, ...own].join(';');
}

/** A built mod's folder is `@` and the name; a list may or may not have been written with the `@`. */
function atOf(name: string): string {
  return name.startsWith('@') ? name : `@${name}`;
}

/** `modsDirectory` as the file that set it means it: a relative path counted from that file. */
function modsDirectoryOf(target: LaunchTarget): string {
  return resolveWindows(target.configuredIn, target.launch.modsDirectory ?? '');
}

/**
 * The profile: under the run folder, and under the target's mod, so that two mods of a monorepo
 * keep their own logs and their own settings rather than writing over each other's.
 */
function profileOf(input: LaunchInput, role: LaunchRole): string {
  const mod = modOf(input)?.name ?? input.target.mod;

  return windowsPath(input.runRoot, PROFILES_FOLDER, mod, role);
}

/** The mod the target names, as the workspace has it. */
function modOf(input: LaunchInput): LaunchMod | undefined {
  return input.mods.find((mod) => sameName(mod.name, input.target.mod));
}
