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
 * A target says what to put up — the client, the server, or both — and both come up out of the one
 * launch: the server first, and a client that joins it on the machine it was started on. The
 * profile and the mission are laid down out of the **target's own mod**, layer by layer, and the
 * `server.cfg` comes from that mod too, falling back to the one beside the file that owns the
 * launch block. Which is what makes a target the same launch on somebody else's machine: what a
 * neighbouring mod happens to keep in its `Missions` has no say in it.
 *
 * The build says which game those processes are. `Debug` is everything above: the diag build, the
 * mirror, the script log off the debugger port. `Release` is the pair a player and a host run,
 * started where they are installed and given none of the diag arguments — no mirror is built for
 * it, because a retail game reads its mods out of the pbo that were packed and out of nothing
 * else. Which is the point of having the choice: a launch that loads the built artefact is the
 * only one that says whether the artefact is the mod.
 *
 * As with a build, the whole of it comes out as a plan — folders, links, copies, command lines —
 * and nothing here goes near a disk or a process. What the disk holds (what is in the game's root,
 * what is in the run folder already, which of the paths of `launchPathsOf` are there) is handed in
 * as plain data, which is what lets the plan be compared whole in a test.
 */

import { sameName } from './config';
import type { Launch, Run, Target } from './enf';
import { type GameBuild, type GameProgram, type GameSide, missingProgramOf } from './machine';
import type { MachineSettings } from './machine';
import {
  type LoadedModName,
  type ModName,
  isModNameOf,
  loadedModNameOf,
  loadedModNameProblemOf,
  modNameProblemOf,
  modPathOf,
} from './modName';
import { resolveWindows, samePath, windowsPath } from './paths';
import { FORCED_SCRIPT_DEBUG_PORT, SCRIPT_DEBUG_HOST } from './scriptDebug';
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

/**
 * The game's root is mirrored one folder in rather than into the run folder itself, and the
 * profile and the mission sit beside that folder rather than in it.
 *
 * Because the game's root holds a `Missions` of its own, and a launch builds a `missions` of its
 * own: on a filesystem that tells neither name apart, one of the two silently becomes the other.
 * Either the junction cannot be made and the game loses its own missions, or — worse, and the way
 * round that happens on the second launch — the mission is written *through* the junction into the
 * DayZ installation, which is the one thing ADR-0001 promises never happens. Nothing of ours goes
 * inside the folder that is mirrored, and then no folder the engine grows can ever collide with it.
 */
const PATCHED_FOLDER = 'game';

/** The folder the game is actually started in: the mirror, inside the run folder. */
export function filePatchingRootOf(runRoot: string): string {
  return windowsPath(runRoot, PATCHED_FOLDER);
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
  /** Whether this target starts DayZ Experimental rather than the stable applications. */
  readonly experimental: boolean;
  /** The effective lists: the target's own where present, otherwise the launch block's. */
  readonly clientMods: readonly string[];
  readonly serverMods: readonly string[];
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
    experimental: target.experimental,
    clientMods: target.clientMods ?? source.launch.clientMods,
    serverMods: target.serverMods ?? source.launch.serverMods,
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

/** A mod as a launch sees it: what it is called, where it is, and what it packs into. */
export interface LaunchMod {
  readonly name: string;
  /** The capability required by every name-derived launch path. */
  readonly modName: ModName | undefined;
  /** The mod root the way Windows takes it: the folder `Missions` and `Profiles` sit in. */
  readonly root: string;
  /** The prefix root the way Windows takes it, which is what the run folder links to. */
  readonly prefixRoot: string;
  /** Its addons by pbo name, which is what tells a mod that is built from one that is not. */
  readonly addons: readonly string[];
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

/**
 * A folder laid over another: one layer of a profile, or of the mission the dev server loads. A
 * source that is not there is ordinary — no mod keeps every layer — and the copying is additive,
 * so the order the layers come in is the order they win in.
 */
export interface FolderCopy {
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
 * What of the game's root is *not* carried into the run folder beside the links.
 *
 * The engine reads several files out of its working directory rather than out of the folder its
 * executable sits in: `DayZSetting.xml`, and `dayz.gproj`, without which it gets as far as
 * "Cannot find game project settings!" and then "Failed to create Enfusion engine" — a server that
 * exits with an access violation and says nothing a developer could act on.
 *
 * Which files those are is not something to write down. This was a list of names once, and the
 * list held one name and was wrong for the same reason the hardcoded folder list was wrong: nobody
 * finds out what is missing until a patch or a machine has it. So the root is listed, exactly as
 * it is listed for the links, and what is skipped is named instead — the programs and libraries
 * the loader takes from beside the executable, which are hundreds of megabytes and are found
 * through the path the game was started by, and the logs the game itself wrote there.
 */
const NOT_CARRIED = /\.(exe|dll|log)$/i;

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

  return { root: input.root, junctions, remove, copies: carriedOf(input), conflicts };
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
    const name = checkedModNameOf(mod);
    if (name === undefined) {
      continue;
    }

    wanted.set(name.toLowerCase(), {
      path: modPathOf(input.root, name, 'source'),
      target: mod.prefixRoot,
    });
  }

  return wanted;
}

function carriedOf(input: FilePatchingInput): FileCopy[] {
  return input.entries
    .filter((entry) => !entry.directory && !NOT_CARRIED.test(entry.name))
    .map((entry) => ({
      from: windowsPath(input.game, entry.name),
      to: windowsPath(input.root, entry.name),
    }));
}

/** The game's installation as a launch reads it: where it is, what starts it, what it holds. */
export interface GameRoot {
  /** The DayZ installation, which is the folder the mirror is made of. */
  readonly path: string;
  /** What each side of this launch starts, already chosen for the build being launched. */
  readonly programs: Readonly<Record<GameSide, GameProgramFacts>>;
  readonly entries: readonly GameEntry[];
}

/** One program of a launch, and whether the disk has it. */
export interface GameProgramFacts extends GameProgram {
  /** Whether it is actually there, which is a fact about the disk. */
  readonly present: boolean;
}

/** Everything a launch is planned from. */
export interface LaunchInput {
  readonly target: LaunchTarget;
  /** Which build of the game this launch is of: the diag one, or the pair a player runs. */
  readonly build: GameBuild;
  /** The mods of the workspace, in the order the model put them in. */
  readonly mods: readonly LaunchMod[];
  readonly settings: MachineSettings;
  readonly drive: WorkDrive;
  /** The folder this workspace's launches are built in: the mirror, the profiles and the mission. */
  readonly runRoot: string;
  readonly game: GameRoot;
  /** What the file patching root holds now, by name. */
  readonly present: ReadonlyMap<string, LinkFact>;
  /**
   * Which of the paths of `launchPathsOf` the disk answered yes to. Whether a pbo is there and
   * whether there is a `server.cfg` to start with are facts about the disk, so they are handed in
   * rather than looked up — the same way the environment is built in `machine.ts`.
   */
  readonly found: readonly string[];
  /**
   * The port each role's process is told to open its script debugger connection on, which is the
   * port a listener of ours is already bound to. Handed in for the same reason the rest of this
   * is: which port was free is a fact about the machine, and a plan does not go and find out.
   */
  readonly debugPorts: Readonly<Record<LaunchRole, number>>;
}

/**
 * Which of a launch's processes this is.
 *
 * `client2` is a second client on the same machine, and it is a role of its own rather than a
 * second `client` because the engine treats it as one: told `-client2`, it dials the debugger on
 * a port of its own, so the two are told apart by the socket their log arrives on. It keeps its
 * own profile for the same reason a server does — two processes writing one profile write over
 * each other's settings and logs.
 *
 * It is never part of what a target puts up. A second client is asked for while the first is
 * already playing, which is a button rather than a plan.
 */
export type LaunchRole = 'client' | 'server' | 'client2';

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
  /** Folders to make: the run folder, the profiles, and the mission the dev server loads. */
  readonly folders: readonly string[];
  /** The profile and the mission, laid down layer by layer out of the target's own mod. */
  readonly copies: readonly FolderCopy[];
  /** The processes to start, in the order they are started: the server before the client. */
  readonly processes: readonly LaunchProcess[];
}

/**
 * What only the diag build is given, and only a `Debug` launch therefore has.
 *
 * All three are the diag build's own. `-filePatching` is what makes the engine read an addon's
 * files off the disk instead of out of the pbo, and it is the whole reason the mirror exists;
 * `-scriptDebug=true` is what opens the debugger the script log comes down; and
 * `-newErrorsAreWarnings=1` turns a script error into a line rather than a stop.
 *
 * A retail game is given none of them, and that is the difference the choice is for. Passing
 * `-filePatching` to it would be asking a launch that exists to run the packed pbo to prefer
 * whatever is lying unpacked beside them, which is the one thing it must not do.
 */
const DIAG_ARGUMENTS: readonly string[] = [
  '-filePatching',
  '-scriptDebug=true',
  '-newErrorsAreWarnings=1',
];

/**
 * The arguments the client is started with whichever build it is. The logging is half the point of
 * launching from here — a script error belongs in a log the developer reads rather than in a
 * message the game swallows — and `-window` is what makes it possible to alt-tab back to the
 * editor that started it.
 */
const CLIENT_ARGUMENTS: readonly string[] = [
  '-doLogs',
  '-adminlog',
  '-nopause',
  '-nosplash',
  '-window',
];

/**
 * And the server's. `-server` is what makes the diag executable a server, and is harmless on the
 * one that is only ever a server; `-world=none` keeps the engine from loading a world of its own,
 * because the world a dev server runs is the one its mission names.
 */
const SERVER_ARGUMENTS: readonly string[] = [
  '-doLogs',
  '-adminlog',
  '-nopause',
  '-nosplash',
  '-world=none',
];

/**
 * The name each client plays under, and the profile it plays out of.
 *
 * `-name` is the engine's profile name, and the profile name is the player: with nothing said, the
 * engine falls back to `Survivor` for both, and a second one on the same server comes up as
 * `Survivor (2)` — two players nobody watching a log or a screen can tell apart. Which is exactly
 * what a second client is for: two of them in the same world, doing different halves of the thing
 * being tested.
 *
 * `A` and `B` rather than anything cleverer because the name has to survive being read at a
 * glance, in an `.ADM` line and over a character's head, and because it belongs to the role rather
 * than to the developer: the same launch on another machine names the same two players.
 *
 * Found by measurement rather than from documentation, which lists no such parameter for DayZ:
 * a server started with `-name=X` writes its profile into `Users\X` where it otherwise writes
 * `Users\Survivor`. `-gamertag`, which the engine does list, does nothing here.
 */
const PLAYER_NAME: Readonly<Record<'client' | 'client2', string>> = {
  client: 'SurvivorA',
  client2: 'SurvivorB',
};

/** The machine the client joins, which for a server this launch put up is the one it is on. */
const LOCAL_ADDRESS = '127.0.0.1';

/** The port the dev server listens on: the one DayZ has answered on since it shipped. */
const DEFAULT_PORT = 2302;

/** The folder the game writes its logs, its `.RPT` and the player's own settings into. */
const PROFILES_FOLDER = 'profiles';

/** And the one the mission the dev server loads is assembled in, beside the profiles. */
const MISSIONS_FOLDER = 'missions';

/** The folders of a mod the profile and the mission are laid down from. */
const PROFILES_SOURCE = 'Profiles';
const MISSIONS_SOURCE = 'Missions';

/** The file a server is configured by, which a target that names none is looked for beside. */
const SERVER_CONFIG = 'server.cfg';

/**
 * The layers a profile is laid down from, in the order they are copied: what belongs to every
 * profile of the mod, what belongs to a developer's rather than to a live server's, and what
 * belongs to the one role. The later one wins, which is what makes a layer worth having over a
 * folder copied once and then edited in two places ever after.
 */
const PROFILE_LAYERS: Readonly<Record<LaunchRole, readonly string[]>> = {
  client: ['Global', 'Dev', 'Client'],
  server: ['Global', 'Dev', 'Server'],
  // The same layers as the first client, in a folder of its own: what differs between the two is
  // which machine they are signed in from, not what the mod wants a client to be configured like.
  client2: ['Global', 'Dev', 'Client'],
};

/** The server's profile takes one more: what belongs to the world this target is about. */
const MAPS_LAYER = 'Maps';

/** The mission takes the mod's own mission for the world first, then the layers that amend it. */
const MISSION_LAYERS: readonly string[] = ['Global', 'Dev'];

/** The folder of a built mod the pbo sit in, which is what a launch looks for them in. */
const ADDONS_FOLDER = 'Addons';

/**
 * The processes a target puts up, in the order they are started. The server goes first: a client
 * with nothing to connect to falls back to the main menu, and by the time a client has finished
 * loading, a server started beside it has long been listening.
 */
function rolesOf(run: Run): LaunchRole[] {
  switch (run) {
    case 'client':
      return ['client'];
    case 'server':
      return ['server'];
    case 'both':
      return ['server', 'client'];
  }
}

/**
 * What the plan wants a yes or a no from the disk about before it can be made: the pbo of every
 * mod it would load, and — where a server is being put up — the `server.cfg` it would start with
 * and the mission it would lay down. Asked here and answered in `LaunchInput.found`, so that the
 * plan itself stays a function of plain data.
 */
export function launchPathsOf(target: LaunchTarget, mods: readonly LaunchMod[]): string[] {
  const roles = rolesOf(target.run);
  const targetName = targetModNameOf(target, mods);
  const built =
    modsDirectoryOf(target) === ''
      ? []
      : loadedNamesOf(target, roles).flatMap((name) => builtPathsOf(target, mods, name));

  const server =
    roles.includes('server') && targetName !== undefined
      ? [...serverConfigsOf(target, mods), missionTemplateOf(target, mods)]
      : [];

  return unique([...built, ...server]);
}

/**
 * The plan: the run folder made ready, the profile and the mission laid down in it, and the game
 * started in it.
 *
 * A refusal leaves no steps at all — half a launch is a game that comes up without the mod it was
 * launched for, which is the failure this exists to prevent rather than to cause.
 */
export function launchPlanOf(
  input: LaunchInput,
  /**
   * Which of the target's processes to plan, where that is not all of them. The second client is
   * the one caller that asks: it is added to a launch that is already up, so it wants the profile
   * and the command line of one role and none of the rest.
   */
  only?: readonly LaunchRole[],
): LaunchPlan {
  const roles = only ?? rolesOf(input.target.run);
  const patched = filePatchingRootOf(input.runRoot);
  const refusals = refusalsOf(input, roles);
  if (refusals.length > 0) {
    return {
      refusals,
      warnings: [],
      filePatching: nothing(patched),
      folders: [],
      copies: [],
      processes: [],
    };
  }

  // The mirror is what `-filePatching` reads, so a launch that is not file-patching has no use for
  // one. Not merely no use: making it would be several hundred junctions and a folder the size of
  // the installation, laid down for a game that will not look at any of it.
  const patching = input.build === 'Debug';
  const filePatching = patching
    ? filePatchingPlanOf({
        root: patched,
        game: input.game.path,
        entries: input.game.entries,
        mods: input.mods,
        present: input.present,
      })
    : nothing(patched);

  const profile = (role: LaunchRole): string => profileOf(input, role);
  const mission = missionOf(input);
  const server = roles.includes('server');
  // An addition to a launch that is already up makes nothing the launch already made. The run
  // folder is there, its links are made, and the game is holding the files that were copied into
  // it — copying them again is refused by Windows, which is exactly how this was found.
  const addition = only !== undefined;
  const makes = patching && !addition;

  return {
    refusals: [],
    warnings: addition ? [] : warningsOf(input, filePatching, roles),
    filePatching: addition ? nothing(patched) : filePatching,
    folders: [...(makes ? [patched] : []), ...roles.map(profile), ...(server ? [mission] : [])],
    copies: [
      ...roles.flatMap((role) => profileCopiesOf(input, role, profile(role))),
      ...(server ? missionCopiesOf(input, mission) : []),
    ],
    processes: roles.map((role) =>
      role === 'server'
        ? serverProcessOf(input, profile('server'), mission)
        : clientProcessOf(input, profile(role), role),
    ),
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
function refusalsOf(input: LaunchInput, roles: readonly LaunchRole[]): string[] {
  const said: string[] = [
    ...gameRefusalOf(input, roles),
    ...driveRefusalOf(input.drive),
    ...modNameRefusalsOf(input.mods),
    ...loadedModNameRefusalsOf(input.target, roles),
  ];

  if (modOf(input.target, input.mods) === undefined) {
    said.push(
      `${input.target.name} launches ${input.target.mod}, which is not a mod of this workspace.`,
    );
  }

  if (modsDirectoryOf(input.target) === '') {
    said.push(
      `No mods directory is set: give ${input.target.configuredBy} a "launch" block with a ` +
        '"modsDirectory", which is where the built mods are loaded from.',
    );
  } else {
    said.push(...unbuiltOf(input, roles));
  }

  if (roles.includes('server')) {
    said.push(...serverRefusalsOf(input));
  }

  said.push(...unquotableOf(input.target));

  return said;
}

function modNameRefusalsOf(mods: readonly LaunchMod[]): string[] {
  return mods.flatMap((mod) => {
    if (checkedModNameOf(mod) !== undefined) {
      return [];
    }

    const problem =
      modNameProblemOf(mod.name) ??
      'The checked mod name no longer matches the name shown for it.';
    return [`${JSON.stringify(mod.name)} cannot be file-patched for this launch. ${problem}`];
  });
}

function loadedModNameRefusalsOf(
  target: LaunchTarget,
  roles: readonly LaunchRole[],
): string[] {
  return uniqueValues(loadedNamesOf(target, roles)).flatMap((written) => {
    const problem = loadedModNameProblemOf(written);
    return problem === undefined ? [] : [`${target.configuredBy}: ${problem}`];
  });
}

/**
 * The programs this launch would start, and whether they are there.
 *
 * Only the sides being put up: a client-only target on a machine with no DayZ Server installed is
 * a launch that goes ahead, and telling it about a program it was never going to start would send
 * a developer off to install something they do not need.
 */
function gameRefusalOf(input: LaunchInput, roles: readonly LaunchRole[]): string[] {
  if (
    input.game.path === '' &&
    (input.target.experimental || input.settings.executable === '')
  ) {
    return input.target.experimental
      ? [
          'No DayZ Experimental installation is set: fill in ' +
            'enfusion.dayzExperimental.path, which is otherwise read from Steam.',
        ]
      : [
          'No DayZ installation is set: fill in enfusion.dayz.path, which is otherwise read from ' +
            'the registry its installer wrote it to.',
        ];
  }

  return sidesOf(roles).flatMap((side) => {
    const program = input.game.programs[side];

    // The path where there is one, and the bare name where nothing said which installation it
    // would come out of: "DayZServer_x64.exe is not there" is still the sentence to read.
    return program.present
      ? []
      : [
          missingProgramOf(
            input.build,
            side,
            program.path === '' ? program.name : program.path,
            input.target.experimental,
          ),
        ];
  });
}

/** Which programs a set of roles is started with: the two clients are the one client program. */
function sidesOf(roles: readonly LaunchRole[]): GameSide[] {
  const sides: GameSide[] = [];

  if (roles.some((role) => role !== 'server')) {
    sides.push('client');
  }

  if (roles.includes('server')) {
    sides.push('server');
  }

  return sides;
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
 * The mods this launch would load that are not there to be loaded.
 *
 * The game comes up regardless — without the mod, and with whatever depended on it failing in a
 * script error — and that reads as a bug in the mod rather than as a mod that was never built.
 * Which is an hour to tell apart, and one sentence to prevent.
 */
function unbuiltOf(input: LaunchInput, roles: readonly LaunchRole[]): string[] {
  const found = foundOf(input);
  const said: string[] = [];

  for (const written of loadedNamesOf(input.target, roles)) {
    const name = loadedModNameOf(written);
    if (name === undefined) {
      continue;
    }

    const ours = ourModOf(input.mods, name);

    // A third-party mod is known by the name of its folder and nothing else — no sources, no addon
    // names — so the folder being there is the whole of what can be asked about it.
    if (ours === undefined) {
      const built = builtModOf(input.target, name);
      if (!found.has(samePath(built))) {
        said.push(`@${name} is not in the mods directory: nothing is at ${built}.`);
      }
      continue;
    }

    const missing = pbosOf(input.target, ours).filter((pbo) => !found.has(samePath(pbo)));
    if (missing.length > 0) {
      said.push(
        `${ours.name} is not built: nothing is at ${missing[0]}. Build it and launch again.`,
      );
    }
  }

  return said;
}

/** What a server needs and a client does not: a world to load, and the file it loads it by. */
function serverRefusalsOf(input: LaunchInput): string[] {
  const target = input.target;
  const said: string[] = [];

  if (mapOf(target) === '') {
    said.push(
      `${target.name} puts up a server, and a server loads a mission of a world: give the target ` +
        'a "map".',
    );
  }

  if (serverConfigOf(input) === undefined) {
    const looked = serverConfigsOf(target, input.mods).join(', nor at ');

    said.push(
      target.serverConfig === undefined
        ? `${target.name} has no ${SERVER_CONFIG} to start the server with: nothing is at ${looked}.`
        : `${target.name} names a "serverConfig" that is not there: nothing is at ${looked}.`,
    );
  }

  return said;
}

/**
 * A quotation mark in what the manifest puts on a command line. No Windows path holds one, so
 * nothing is lost by refusing it — and an argument that ends where nobody meant it to is worth
 * refusing before it reaches a process rather than after.
 */
function unquotableOf(target: LaunchTarget): string[] {
  const value = [
    target.launch.modsDirectory ?? '',
    ...target.clientMods,
    ...target.serverMods,
    target.serverConfig ?? '',
  ].find((text) => text.includes('"'));

  return value === undefined
    ? []
    : [`${target.configuredBy} has a quotation mark in "${value}", which no path can hold.`];
}

/** What was asked for and will not happen, though the launch goes ahead regardless. */
function warningsOf(
  input: LaunchInput,
  filePatching: FilePatchingPlan,
  roles: readonly LaunchRole[],
): string[] {
  const said: string[] = [];

  // The layers on their own still make a mission the engine will load, so this is a sentence
  // rather than a stop — but a server coming up on an empty world when a whole map was meant is
  // worth one.
  if (roles.includes('server')) {
    const template = missionTemplateOf(input.target, input.mods);

    if (!foundOf(input).has(samePath(template))) {
      said.push(
        `Nothing is at ${template}, so the server starts with whatever the layers of ` +
          `${MISSIONS_SOURCE} hold and no mission of ${input.target.mod}'s own.`,
      );
    }
  }

  for (const path of filePatching.conflicts) {
    said.push(`${path} is not a link of ours, so it is left as it is and nothing is patched into it.`);
  }

  return said;
}

/**
 * A client, and — where the role says so — the second one.
 *
 * The second differs in four things and nothing else: `-client2`, which is what makes the engine
 * meet it on a debugger port of its own; a profile of its own, because two clients writing one
 * profile write over each other; a name of its own, so that the two are told apart in the world
 * and in the server's log; and it always joins rather than falling back to an offline mission,
 * since it is only ever started to sit beside a game that is already up.
 */
function clientProcessOf(
  input: LaunchInput,
  profile: string,
  role: 'client' | 'client2',
): LaunchProcess {
  const second = role === 'client2';

  return {
    role,
    what: `Starting the ${second ? 'second client' : 'client'} for ${input.target.name}`,
    program: input.game.programs.client.path,
    arguments: [
      ...diagArgumentsOf(input),
      ...CLIENT_ARGUMENTS,
      ...(second ? ['-client2'] : []),
      ...scriptDebugOf(input, role),
      `-name=${PLAYER_NAME[role]}`,
      `-profiles=${profile}`,
      ...listArgumentOf('-mod', loadedOf(input)),
      ...(second ? joiningOf() : joinOf(input)),
      ...(second ? [] : offlineMissionOf(input)),
    ],
    cwd: workingDirectoryOf(input, 'client'),
  };
}

/**
 * The server. It gets `-serverMod=` out of `serverMods` and nothing out of `clientMods` — the two
 * lists are read independently, so nothing reaches the server for having been named to the client.
 * A mod both sides need is a mod named in both lists.
 */
function serverProcessOf(input: LaunchInput, profile: string, mission: string): LaunchProcess {
  return {
    role: 'server',
    what: `Starting the server for ${input.target.name}`,
    program: input.game.programs.server.path,
    arguments: [
      '-server',
      ...diagArgumentsOf(input),
      ...SERVER_ARGUMENTS,
      ...scriptDebugOf(input, 'server'),
      `-port=${DEFAULT_PORT}`,
      `-config=${serverConfigOf(input) ?? ''}`,
      `-profiles=${profile}`,
      `-mission=${mission}`,
      ...listArgumentOf('-serverMod', pathsOf(input.target, input.target.serverMods)),
    ],
    cwd: workingDirectoryOf(input, 'server'),
  };
}

/** The three the diag build alone understands, and a release launch is given none of. */
function diagArgumentsOf(input: LaunchInput): readonly string[] {
  return input.build === 'Debug' ? DIAG_ARGUMENTS : [];
}

/**
 * Where the process is started.
 *
 * A `Debug` launch runs in the mirror, because that is what `-filePatching` counts an addon's
 * prefix from. A `Release` one runs where the game is installed: there is no mirror to run in, and
 * a retail game started anywhere else is one that cannot find its own root files.
 */
function workingDirectoryOf(input: LaunchInput, side: GameSide): string {
  return input.build === 'Debug'
    ? filePatchingRootOf(input.runRoot)
    : input.game.programs[side].root;
}

/**
 * Where the process is to dial its script debugger, which is a listener of ours on this machine.
 *
 * The host is given as a number rather than left to the default `localhost`, because the default
 * goes through a resolver and the engine's connect is IPv4 only — a machine whose `localhost`
 * answers `::1` first would spend the whole launch failing to connect to us.
 *
 * The port is only said to a role that is listened to. A server writes over whatever
 * `-debuggerPort` gave it the moment it knows it is a server, so telling it a port would be a line
 * on the command line that means nothing and reads as though it does. See
 * `FORCED_SCRIPT_DEBUG_PORT`.
 *
 * Each role is met on its own port either way, so which process is talking is a fact about which
 * socket it came in on rather than something to work out. The connection does say its process id,
 * which is worth having as a check, but it is not what the two are told apart by.
 *
 * A release build carries no debugger at all, so it is told of none: the pair of arguments would
 * name a listener nothing was ever going to dial, and a command line in the log that reads as
 * though a script log is coming is worse than one that does not.
 */
function scriptDebugOf(input: LaunchInput, role: LaunchRole): string[] {
  if (input.build !== 'Debug') {
    return [];
  }

  const host = `-debugger=${SCRIPT_DEBUG_HOST}`;

  return FORCED_SCRIPT_DEBUG_PORT[role] === undefined
    ? [host, `-debuggerPort=${input.debugPorts[role]}`]
    : [host];
}

/**
 * A list of mods as one argument, or no argument at all where the list is empty. An empty `-mod=`
 * is not the same thing as no `-mod=`: the game takes it badly, and the previous implementation
 * left it off for that same reason.
 */
function listArgumentOf(name: string, paths: readonly string[]): string[] {
  return paths.length === 0 ? [] : [`${name}=${paths.join(';')}`];
}

/** The client joins the server this same launch put up, which is what makes `both` one launch. */
function joinOf(input: LaunchInput): string[] {
  return input.target.run === 'both' ? joiningOf() : [];
}

/** Where to find the server: on this machine, on the port a dev server has always answered on. */
function joiningOf(): string[] {
  return [`-connect=${LOCAL_ADDRESS}`, `-port=${DEFAULT_PORT}`];
}

/**
 * A client with no server to join loads an offline mission of the target's world, which is what
 * makes a target of one map worth having at all. With no map it comes up at the main menu.
 */
function offlineMissionOf(input: LaunchInput): string[] {
  const map = mapOf(input.target);

  return input.target.run === 'client' && map !== '' ? [`-mission=dayzOffline.${map}`] : [];
}

/**
 * The mods every client process loads: `clientMods`, the list the manifest wrote, in the order it
 * wrote it, and nothing else. The server never sees this list — see `serverProcessOf`.
 *
 * Nothing of the workspace is added to it. A mod of ours reaches the command line because it was
 * named in `clientMods`, exactly the way a third-party one does. It was once the other way round —
 * every mod of the workspace was appended to whatever the manifest said — and that had the two
 * faults a developer meets in the same afternoon: naming one of ours to move it earlier loaded it
 * twice instead of moving it, and a workspace that held a mod no launch wanted had no way of
 * leaving it out.
 */
function loadedOf(input: LaunchInput): string[] {
  return pathsOf(input.target, input.target.clientMods);
}

/**
 * Every mod this launch names, so a plan can ask the disk whether it was built: `clientMods` where
 * a client-type process is going up, `serverMods` where the server is, the union where both are —
 * never one list standing in for the other, because the two no longer share a command line.
 */
function loadedNamesOf(target: LaunchTarget, roles: readonly LaunchRole[]): string[] {
  const client = roles.some((role) => role !== 'server');
  const server = roles.includes('server');

  return [
    ...(client ? target.clientMods : []),
    ...(server ? target.serverMods : []),
  ];
}

/**
 * What says a named mod is there to load: its pbos where the workspace holds its sources, and the
 * folder itself where it does not.
 *
 * One of ours is worth the stricter question. The workspace knows what it packs into, so a mod
 * that was never built can be told from one that was, and named by the pbo that is missing rather
 * than by a folder — which is the difference between a developer building it and a developer
 * wondering what the launch wants. A mod that is only a folder name in the mods directory can be
 * asked nothing else.
 */
function builtPathsOf(target: LaunchTarget, mods: readonly LaunchMod[], name: string): string[] {
  const checked = loadedModNameOf(name);
  if (checked === undefined) {
    return [];
  }

  const ours = ourModOf(mods, checked);

  return ours === undefined ? [builtModOf(target, checked)] : pbosOf(target, ours);
}

/** The workspace mod a reference names; whether that mod has a usable path is decided afterwards. */
function ourModOf(mods: readonly LaunchMod[], name: LoadedModName): LaunchMod | undefined {
  return mods.find((mod) => sameName(mod.name, name));
}

/** Every one of them as the folder the game is pointed at. */
function pathsOf(target: LaunchTarget, names: readonly string[]): string[] {
  return names.flatMap((written) => {
    const name = loadedModNameOf(written);
    return name === undefined ? [] : [builtModOf(target, name)];
  });
}

/** `<ModsDirectory>\@<Name>`: the built mod, which is what is loaded rather than the sources. */
function builtModOf(target: LaunchTarget, name: ModName | LoadedModName): string {
  return modPathOf(modsDirectoryOf(target), name, 'built');
}

/** The pbo a mod is built into, one per addon: the files that say whether it was built at all. */
function pbosOf(target: LaunchTarget, mod: LaunchMod): string[] {
  const name = checkedModNameOf(mod);
  if (name === undefined) {
    return [];
  }

  return mod.addons.map((addon) =>
    windowsPath(builtModOf(target, name), ADDONS_FOLDER, `${addon}.pbo`),
  );
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
  const name = targetModNameOf(input.target, input.mods);
  if (name === undefined) {
    return '';
  }

  return windowsPath(
    modPathOf(profilesRootOf(input.settings.profiles, input.runRoot), name, 'source'),
    role,
  );
}

/**
 * Where the profiles are built: the folder the settings name, or the one in the run folder.
 *
 * It is the one part of a launch a developer reads rather than merely runs — the `.RPT`, the
 * `.ADM`, whatever a server mod keeps its configuration in — so where it lands is worth being
 * able to say. Under it the layout is `<Mod>\<role>`, which is the layout the Workbench plugins
 * use as well: pointed at the same folder they point at, both toolchains write one profile
 * instead of two that drift.
 *
 * Only the profiles move. The file patching root stays in the run folder whatever this says,
 * because it is a mirror of the whole game installation and the work drive is the one place it
 * must never be: pboProject and AddonBuilder both read what is on that drive, and AddonBuilder
 * binarises with `-addon="P:"`, which is every config on it.
 */
export function profilesRootOf(configured: string, runRoot: string): string {
  return configured.trim() === ''
    ? windowsPath(runRoot, PROFILES_FOLDER)
    : configured.trim();
}

/**
 * The mission the dev server loads, assembled under the run folder. Not on the work drive and not
 * in the mod: it is made afresh out of the layers every launch, and a folder that is written to
 * every launch has no business sitting where the sources are.
 */
function missionOf(input: LaunchInput): string {
  return windowsPath(input.runRoot, MISSIONS_FOLDER, missionNameOf(input.target, input.mods));
}

/** `CADCore.chernarusplus`: the mod, and the world it is being launched on. */
function missionNameOf(target: LaunchTarget, mods: readonly LaunchMod[]): string {
  const name = targetModNameOf(target, mods);
  return name === undefined ? '' : `${name}.${mapOf(target)}`;
}

/** The mission the target's mod keeps for that world, which the run's is laid down from. */
function missionTemplateOf(target: LaunchTarget, mods: readonly LaunchMod[]): string {
  return windowsPath(rootOf(target, mods), MISSIONS_SOURCE, missionNameOf(target, mods));
}

/**
 * The profile, layer by layer, out of the target's mod. Out of that mod only: a launch that took
 * the `Profiles` of whatever else the workspace holds would be a different launch on every machine.
 */
function profileCopiesOf(input: LaunchInput, role: LaunchRole, profile: string): FolderCopy[] {
  const root = windowsPath(rootOf(input.target, input.mods), PROFILES_SOURCE);
  const map = mapOf(input.target);
  const layers = [
    ...PROFILE_LAYERS[role],
    ...(role === 'server' && map !== '' ? [windowsPath(MAPS_LAYER, map)] : []),
  ];

  return layers.map((layer) => ({ from: windowsPath(root, layer), to: profile }));
}

/** And the mission: the mod's own for this world first, then the layers that amend it. */
function missionCopiesOf(input: LaunchInput, mission: string): FolderCopy[] {
  const root = windowsPath(rootOf(input.target, input.mods), MISSIONS_SOURCE);

  return [
    { from: missionTemplateOf(input.target, input.mods), to: mission },
    ...MISSION_LAYERS.map((layer) => ({ from: windowsPath(root, layer), to: mission })),
  ];
}

/**
 * Where the `server.cfg` is looked for, in the order it is looked for in: the one the target
 * names, or the one the target's mod keeps, or the one beside the file that owns the launch block.
 * A monorepo configures its dev server once that way, and a mod that wants its own says so by
 * keeping one.
 */
function serverConfigsOf(target: LaunchTarget, mods: readonly LaunchMod[]): string[] {
  const named = target.serverConfig?.trim() ?? '';
  if (named !== '') {
    return [resolveWindows(rootOf(target, mods), named)];
  }

  return unique([
    windowsPath(rootOf(target, mods), SERVER_CONFIG),
    windowsPath(target.configuredIn, SERVER_CONFIG),
  ]);
}

/** The first of those that is actually there; undefined is what the server is refused over. */
function serverConfigOf(input: LaunchInput): string | undefined {
  const found = foundOf(input);

  return serverConfigsOf(input.target, input.mods).find((path) => found.has(samePath(path)));
}

/** What the disk answered yes to, ready to be compared the way Windows compares a path. */
function foundOf(input: LaunchInput): Set<string> {
  return new Set(input.found.map(samePath));
}

/** The mod the target names, as the workspace has it. */
function modOf(target: LaunchTarget, mods: readonly LaunchMod[]): LaunchMod | undefined {
  return mods.find((mod) => sameName(mod.name, target.mod));
}

/** Its checked name as the workspace spells it; an unknown or invalid target has no path name. */
function targetModNameOf(target: LaunchTarget, mods: readonly LaunchMod[]): ModName | undefined {
  const mod = modOf(target, mods);
  return mod === undefined ? undefined : checkedModNameOf(mod);
}

/** A prepared launch name is usable only while it still belongs to the raw model value. */
function checkedModNameOf(mod: LaunchMod): ModName | undefined {
  const name = mod.modName;
  return isModNameOf(mod.name, name) ? name : undefined;
}

/** And its root, which is the folder the profile and the mission are taken out of. */
function rootOf(target: LaunchTarget, mods: readonly LaunchMod[]): string {
  return modOf(target, mods)?.root ?? '';
}

/** The world, however it was written; empty where the target names none. */
function mapOf(target: LaunchTarget): string {
  return target.map?.trim() ?? '';
}

/** The same path asked about twice is one question, and Windows tells neither spelling apart. */
function unique(paths: readonly string[]): string[] {
  const seen = new Set<string>();

  return paths.filter((path) => {
    const key = samePath(path);
    if (path === '' || seen.has(key)) {
      return false;
    }

    seen.add(key);
    return true;
  });
}

/** Text values deduplicated without dropping an empty invalid value as path deduplication does. */
function uniqueValues(values: readonly string[]): string[] {
  const seen = new Set<string>();

  return values.filter((value) => {
    const key = value.toLowerCase();
    if (seen.has(key)) {
      return false;
    }
    seen.add(key);
    return true;
  });
}
