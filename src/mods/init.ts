/**
 * Starting a mod, starting a workspace, adopting a mod that is already there, and adding an addon.
 *
 * A mod is a folder with `mod.enf`, holding a prefix root of the mod's own name, and inside that
 * the addons. Getting one of those wrong — a `dir` that does not match the folder, a script module
 * path in the wrong case, a `CfgPatches` nobody requires — is the sort of mistake that shows up as
 * a mod which packs cleanly and then does nothing in the game, so none of it is typed by hand
 * here: every name in every file is worked out from the one name the developer gave.
 *
 * Where a mod may be started is decided here too, and it is decided before anything is written:
 * a mod is one mod, so it is never made inside another, nor over the top of mods that are there
 * already. A folder holding several of them is a workspace, and a mod started in a workspace goes
 * into a folder of its own in it.
 *
 * A mod somebody else wrote is the same job with the answers already given: it has a `config.cpp`
 * declaring it and no `mod.enf`, and everything that file would be filled in with — the name, who
 * wrote it, what it does — is written down in the config already. So it is read out of there
 * rather than asked for a second time.
 *
 * The whole of it comes out as a plan — folders to make, files to write, and for an addon the one
 * edit that keeps it from being lost — so that what a new mod is made of can be compared whole in
 * a test instead of by watching what was written. Nothing here goes near a disk.
 *
 * Paths are `/` separated and counted from the mod root, which is the folder `mod.enf` goes in.
 */

import {
  type ConfigCpp,
  type ModDefs,
  type PatchClass,
  modDefsOf,
  parseConfig,
  sameName,
  withRequiredAddon,
} from './config';
import { WORKSPACE_FILE, readWorkspace } from './enf';
import { changesOf } from './form';
import {
  MOD_NAME_PATTERN,
  type ModName,
  modNameOf,
  modNameProblemOf,
} from './modName';
import { CONFIG_FILE, type Layout, MANIFEST_FILE, type Mod, mainAddonOf } from './model';
import { folderOf, isWithin, samePath } from './paths';
import { PROJECT_FILE, PROJECT_FOLDER, projectFileOf } from './workbench';

/** Folders to make and files to write, in the order they are made and written. */
export interface InitPlan {
  /** Every folder, parents before children; the files below make their own as well. */
  readonly folders: readonly string[];
  readonly files: readonly PlannedFile[];
}

export interface PlannedFile {
  /** Under the mod root, `/` separated. */
  readonly path: string;
  readonly content: string;
  /**
   * What happens where the file is there already, which for every other file is a refusal. `lines`
   * writes into it the lines it lacks and keeps the ones it has: a `.gitignore`, which a repository
   * has before it has a mod — and a mod refused over it is a mod nobody can start in a repository
   * at all. `keep` leaves it as it is: a Workbench project somebody has already made is theirs, and
   * the one written here would only have been a start.
   */
  readonly merge?: 'lines' | 'keep';
}

/** What a new mod depends on besides its name and layout: where it is being made. */
export interface InitPlace {
  /** The work drive's letter, which a Workbench project counts every file from. */
  readonly drive: string;
  /**
   * Whether a `workspace.enf` above the mod owns its launch. Such a mod has no launch block of its
   * own — one would be ignored whole — no `Addons` of its own, because the workspace's
   * `modsDirectory` is where it is built to, and no Workbench project of its own: the
   * workspace's is the one every mod of it is written into.
   */
  readonly inWorkspace: boolean;
}

/** Everything a new addon of a mod takes: the folder and config of it, and one edit. */
export interface AddonPlan extends InitPlan {
  /**
   * The name written into the main addon's `requiredAddons`. An addon nothing requires is one the
   * engine loads whenever it likes, which is how a new addon stops working without a word.
   */
  readonly requires: AddonRequirement | undefined;
  /** Why nothing can be added; the plan is empty when there is one. */
  readonly refusal: string | undefined;
  /** What is being done anyway, and is worth saying out loud. */
  readonly warning: string | undefined;
}

/** Everything an unconfigured mod takes to stop being one: the `mod.enf` it has not got. */
export interface Adoption extends InitPlan {
  /** What the config answered for, which is what the developer is shown before agreeing. */
  readonly fields: ModFields;
  /** The checked name to link after writing; absent when the inferred name itself is invalid. */
  readonly modName: ModName | undefined;
  /** Why nothing can be written; the plan is empty when there is one. */
  readonly refusal: string | undefined;
}

/** What a `mod.enf` says about the mod itself, as far as a `config.cpp` can answer for it. */
export interface ModFields {
  /** Always something: the name the config declares the mod under, or the prefix root's own. */
  readonly name: string;
  readonly description: string | undefined;
  readonly author: string | undefined;
  readonly version: string | undefined;
}

/** One name to write into one `CfgPatches` class of one file. */
export interface AddonRequirement {
  /** The main addon's `config.cpp`, as the model has it. */
  readonly config: string;
  /** The class in it that the name goes into. */
  readonly patch: string;
  /** The new addon's own `CfgPatches` name. */
  readonly required: string;
}

/** Why an addon name will not do; it shares the class-and-folder grammar of an owned mod name. */
export function addonNameProblemOf(name: string): string | undefined {
  if (name.trim() === '') {
    return 'An addon needs a name: it is the folder it is packed out of, and its pbo.';
  }

  return NAME.test(name) ? modNameProblemOf(name) : named('An addon');
}

function named(what: string): string {
  return (
    `${what} is named by a class as well as by a folder: letters, digits and underscores, ` +
    'starting with a letter or underscore.'
  );
}

/** What a class in a `config.cpp` can be called, which a folder can always be called too. */
const NAME = new RegExp(MOD_NAME_PATTERN);

/**
 * Everything a new mod is made of. The layout is the one thing asked besides the name, because it
 * is the one thing that cannot be changed later without rewriting every path in `CfgMods`: a mod
 * of one pbo keeps its `config.cpp` in the prefix root, and a mod of several keeps one per addon.
 *
 * The script module paths come out the same either way — `<Name>/Scripts/<module>` — because in a
 * single-addon mod the prefix root is the addon, and in a multi-addon one the addon is `Scripts`
 * inside it. Which is what lets a mod be split up later by moving files rather than editing paths.
 *
 * A mod made on its own gets a Workbench project of its own, beside the prefix root rather than in
 * it, so that the Workbench button opens it on the spot; a mod made in a workspace is written into
 * the workspace's project instead, which is the caller's to edit (`withProjectMod`).
 */
export function initPlanOf(name: ModName, layout: Layout, place: InitPlace): InitPlan {
  const main = layout === 'single' ? name : `${name}/${SCRIPTS}`;
  // A mod being started has said nothing about itself yet, so every field but its name is left
  // for the developer to answer — which is what an adopted mod's config answers instead.
  const fields: ModFields = { name, description: undefined, author: undefined, version: undefined };
  const config = configOf(name, layout);

  return {
    folders: [
      name,
      `${name}/${SCRIPTS}`,
      ...MODULES.map((module) => `${name}/${SCRIPTS}/${module.folder}`),
      // What never reaches a pbo: the layers a launch lays a profile and a mission down from, the
      // folder the built mod is written into, and the Workbench project.
      MISSIONS,
      `${MISSIONS}/${GLOBAL}`,
      PROFILES,
      `${PROFILES}/${GLOBAL}`,
      `${PROFILES}/${DEV}`,
      ...(place.inWorkspace ? [] : [BUILT, PROJECT_FOLDER]),
    ],
    files: [
      {
        path: MANIFEST_FILE,
        content: place.inWorkspace ? workspaceModManifestOf(name) : manifestOf(name, fields),
      },
      { path: GITIGNORE_FILE, content: GITIGNORE, merge: 'lines' },
      { path: `${main}/${CONFIG_FILE}`, content: config },
      { path: `${name}/${MOD_CPP}`, content: modCppOf(name) },
      // A target that puts up a server is refused without one. A mod of a workspace is launched
      // by the workspace's, which is where a target that names none looks after the mod's root.
      ...(place.inWorkspace
        ? []
        : [{ path: SERVER_CONFIG, content: serverConfigOf(name), merge: 'keep' as const }]),
      { path: `${main}/${STRINGTABLE_FILE}`, content: stringtableOf(name) },
      { path: `${name}/${SCRIPTS}/${INPUTS}`, content: INPUTS_XML },
      ...MODULES.map((module) => ({
        path: `${name}/${SCRIPTS}/${module.folder}/${name}.c`,
        content: moduleFileOf(name, module),
      })),
      // A folder with nothing in it is a folder git does not keep, and these are the folders the
      // developer is meant to fill. `Addons` is left out of this: it holds the built mod, git
      // ignores it anyway, and the build makes it.
      ...[`${MISSIONS}/${GLOBAL}`, `${PROFILES}/${GLOBAL}`, `${PROFILES}/${DEV}`].map((folder) => ({
        path: `${folder}/${KEEP_FILE}`,
        content: '',
      })),
      // Read back out of the config it goes with, so the project lists exactly the folders the
      // game will compile — the way a project for a mod made anywhere else is written.
      ...(place.inWorkspace
        ? []
        : [
            {
              path: `${PROJECT_FOLDER}/${PROJECT_FILE}`,
              content: projectFileOf(place.drive, name, [modDefsOf(config)]),
              merge: 'keep' as const,
            },
          ]),
    ],
  };
}

/**
 * What a mod made in a workspace adds to the workspace's Workbench project: the folders its config
 * attaches, read out of the plan rather than worked out a second time.
 */
export function plannedDefsOf(plan: InitPlan): ModDefs | undefined {
  const config = plan.files.find((file) => file.path.endsWith(`/${CONFIG_FILE}`));

  return config === undefined ? undefined : modDefsOf(config.content);
}

/**
 * The workspace's launch block with a new mod named in its `mods`, which is the one way a mod of a
 * workspace gets loaded: nothing is added to that list on the way to the game. A mod named there
 * already — with its `@` or without — leaves the file as it is.
 *
 * Written the way the form writes a row, so the comments and the layout around it stay; and
 * undefined where the form would not write either — a syntax error, a key written twice — because
 * an edit into a file nobody can aim at is an edit that lands somewhere nobody is looking.
 */
export function withWorkspaceMod(source: string, name: ModName): string | undefined {
  const named = readWorkspace(source).value.launch?.mods ?? [];
  if (named.some((mod) => sameName(mod.replace(/^@/, ''), name))) {
    return source;
  }

  const changes = changesOf('workspace', source, {
    kind: 'append',
    path: ['launch', 'mods'],
    value: `@${name}`,
  });

  // Nothing to change about a mod not named yet is the form refusing the file.
  if (changes.length === 0) {
    return undefined;
  }

  return [...changes]
    .sort((a, b) => b.offset - a.offset)
    .reduce(
      (text, change) => text.slice(0, change.offset) + change.content + text.slice(change.offset + change.length),
      source,
    );
}

/**
 * Everything a new workspace is made of: the `workspace.enf` that owns the launch of every mod to
 * be made under it, the folder they are all built into, a Workbench project they are all written
 * into, and a `.gitignore` for what the build makes. Nothing about any one mod: those are made
 * afterwards, each in a folder of its own, and each named in the launch block as it is.
 */
export function workspacePlanOf(drive: string, title: string): InitPlan {
  return {
    folders: [BUILT, PROJECT_FOLDER],
    files: [
      { path: WORKSPACE_FILE, content: WORKSPACE_MANIFEST },
      { path: SERVER_CONFIG, content: serverConfigOf(title), merge: 'keep' },
      { path: GITIGNORE_FILE, content: BUILD_IGNORED, merge: 'lines' },
      { path: `${PROJECT_FOLDER}/${PROJECT_FILE}`, content: projectFileOf(drive, title, []), merge: 'keep' },
    ],
  };
}

/**
 * A `.gitignore` with the lines it lacks added at its end, the way a developer would add them:
 * under a heading, after everything that was there. What it had stays word for word, so a rule
 * somebody wrote on purpose is never undone by one of ours — a line is only added, never changed.
 */
export function mergedLinesOf(existing: string, wanted: string): string {
  const have = new Set(existing.split(/\r?\n/).map((line) => line.trim()));
  const missing = wanted
    .split('\n')
    .map((line) => line.trim())
    .filter((line) => line !== '' && !line.startsWith('#') && !have.has(line));

  if (missing.length === 0) {
    return existing;
  }

  const newline = existing.includes('\r\n') ? '\r\n' : '\n';
  const ended = existing === '' || existing.endsWith('\n') ? existing : `${existing}${newline}`;
  const spaced = ended === '' ? '' : `${ended}${newline}`;

  return `${spaced}${[MERGED_HEADING, ...missing].join(newline)}${newline}`;
}

/** What the lines added to somebody's `.gitignore` are put under, so it is plain who added them. */
const MERGED_HEADING = '# What an Enfusion mod builds, writes while it runs, and signs with.';

/** A mod of the workspace as far as where another one may go: where it sits, and what it says. */
export interface Placed {
  readonly name: string;
  /** Where its `mod.enf` is; for a mod with none, the prefix root's parent. */
  readonly root: string;
  readonly prefixRoot: string | undefined;
  readonly configured: boolean;
  /** Whether its own `mod.enf` has a launch block, which a workspace above it would take over. */
  readonly launches: boolean;
}

/** What a folder asked to hold a new mod or a new workspace sits among. */
export interface Surroundings {
  readonly mods: readonly Placed[];
  /** Every `workspace.enf` the search found, by path. */
  readonly workspaces: readonly string[];
  /** Whether a path is one a `workspace.enf` leaves out of this window. */
  readonly ignored: (path: string) => boolean;
}

/**
 * Why no mod can be made in this folder, whatever it would be called, or undefined where one can.
 * Asked before the name is, so a developer who picked the wrong folder hears so before typing.
 *
 * Each refusal is something that would otherwise be quietly broken. A mod inside another is packed
 * into the other's pbo and confuses which folder is whose prefix root; a `mod.enf` above mods that
 * are there already makes one mod of all of them; and a mod in a folder the workspace ignores is a
 * mod nothing in this window would ever list, build or launch.
 */
export function modFolderRefusalOf(folder: string, around: Surroundings): string | undefined {
  const here = around.mods.find((mod) => mod.configured && same(mod.root, folder));
  if (here !== undefined) {
    return (
      `There is a mod here already: this folder is ${here.name}, with its ${MANIFEST_FILE}. Pick ` +
      'another folder, or add an addon to the mod that is here.'
    );
  }

  const outer = around.mods.find((mod) => within(folder, mod.root) && !same(mod.root, folder));
  if (outer !== undefined) {
    return (
      `This folder is inside ${outer.name}. A mod made here would sit among another mod's files — ` +
      'packed into its pbo, and taken for part of it — so make the new one in a folder of its own.'
    );
  }

  if (around.ignored(`${folder}/${MANIFEST_FILE}`)) {
    return (
      `This folder is one a ${WORKSPACE_FILE} above it ignores, so a mod made here would never be ` +
      'listed, built or launched from this window. Open the folder in a window of its own and make ' +
      'the mod there.'
    );
  }

  if (isWorkspaceRoot(folder, around)) {
    return undefined;
  }

  const held = around.mods.filter((mod) => within(mod.root, folder) || within(mod.prefixRoot, folder));
  const foreign = held.find((mod) => !mod.configured && same(mod.root, folder));
  if (foreign !== undefined) {
    return (
      `This folder holds ${foreign.name} already, a mod found by its ${CONFIG_FILE}: a second mod ` +
      `made here would take its place. Give it a ${MANIFEST_FILE} with "+ Create mod.enf" instead, ` +
      'or make the new mod in a folder of its own.'
    );
  }

  if (held.length > 0) {
    return (
      `This folder holds ${namesOf(held)} already, and a mod made here would hold ${them(held)} as ` +
      'well: a mod is one mod. Make the new one in a folder of its own, or make this folder a ' +
      'workspace with Create Workspace and make the mod in that.'
    );
  }

  return undefined;
}

/** Where a new mod goes: its own root, and the `workspace.enf` that owns its launch, if any. */
export type ModPlacement =
  | { readonly root: string; readonly workspace: string | undefined; readonly refusal?: undefined }
  | { readonly refusal: string };

/**
 * Where a mod of this name made in this folder goes. A workspace's own folder holds its mods each
 * in a folder of its own, so a mod made there is made in a new one named after it; any other
 * folder is the mod's root itself. What a folder of that name in a workspace already holds is the
 * one thing left to refuse over — the folder the developer picked was asked about already.
 */
export function modPlacementOf(folder: string, name: ModName, around: Surroundings): ModPlacement {
  const root = isWorkspaceRoot(folder, around) ? `${folder}/${name}` : folder;
  const taken = around.mods.find(
    (mod) => within(root, mod.prefixRoot ?? mod.root) || (!same(root, folder) && within(mod.root, root)),
  );

  if (taken !== undefined && !same(root, folder)) {
    return {
      refusal:
        `${name} is a folder of this workspace already, and it is ${taken.name}'s. Give the new ` +
        'mod another name.',
    };
  }

  return { root, workspace: workspaceAbove(root, around.workspaces) };
}

/**
 * Why no workspace can be made in this folder, or undefined where one can. A workspace is the
 * folder above its mods, so it is never made inside one. And a `workspace.enf` owns the launch of
 * every mod under it by existing, so it is not made over mods that launch by blocks of their own,
 * or over mods another workspace launches: either way a launch that works now would stop.
 */
export function workspaceFolderRefusalOf(folder: string, around: Surroundings): string | undefined {
  if (isWorkspaceRoot(folder, around)) {
    return `This folder is a workspace already: it has a ${WORKSPACE_FILE}.`;
  }

  const outer = around.mods.find(
    (mod) => within(folder, mod.root) && (mod.configured || !same(mod.root, folder)),
  );
  if (outer !== undefined) {
    return (
      `This folder is ${same(outer.root, folder) ? '' : 'inside '}${outer.name}. A workspace is ` +
      'the folder above its mods, so make it in the folder that holds them.'
    );
  }

  const under = around.mods.filter((mod) => mod.configured && within(mod.root, folder));
  const launching = under.filter((mod) => mod.launches);
  if (launching.length > 0) {
    return (
      `${namesOf(launching)} ${launching.length === 1 ? 'launches' : 'launch'} by the "launch" ` +
      `block of ${launching.length === 1 ? 'its' : 'their'} own ${MANIFEST_FILE}, and a ` +
      `${WORKSPACE_FILE} here would take that over: ${launching.length === 1 ? 'it' : 'those'} ` +
      'would be ignored whole. Move the launch into a workspace.enf by hand, or make the workspace ' +
      'somewhere that holds none of them.'
    );
  }

  const owner = workspaceAbove(folder, around.workspaces);
  if (owner !== undefined && under.length > 0) {
    return (
      `${namesOf(under)} ${under.length === 1 ? 'is' : 'are'} launched by the ${WORKSPACE_FILE} ` +
      'above this folder, and one here would take them out of it. Make the workspace somewhere ' +
      'that holds none of its mods.'
    );
  }

  return undefined;
}

/** The nearest `workspace.enf` at or above this folder, compared the way Windows compares paths. */
function workspaceAbove(folder: string, workspaces: readonly string[]): string | undefined {
  return workspaces
    .filter((file) => within(folder, folderOf(file)))
    .sort((a, b) => folderOf(b).length - folderOf(a).length)
    .at(0);
}

function isWorkspaceRoot(folder: string, around: Surroundings): boolean {
  return around.workspaces.some((file) => same(folderOf(file), folder));
}

function same(a: string, b: string): boolean {
  return samePath(a) === samePath(b);
}

/** A folder within another, or the other itself; a folder that is not there is within nothing. */
function within(folder: string | undefined, root: string): boolean {
  return folder !== undefined && isWithin(samePath(folder), samePath(root));
}

function namesOf(mods: readonly Placed[]): string {
  const names = mods.map((mod) => mod.name);
  const last = names.at(-1) ?? '';

  return names.length <= 1 ? last : `${names.slice(0, -1).join(', ')} and ${last}`;
}

function them(mods: readonly Placed[]): string {
  return mods.length === 1 ? 'it' : 'them';
}

/**
 * A mod found by its `config.cpp` alone, given the `mod.enf` it has not got. Only that one file is
 * written: everything else about the mod — its prefix root, its addons, whatever it keeps beside
 * them — is somebody's work that is already there and that adoption has no business touching.
 *
 * The layout has nothing to say here. A single-addon mod declares itself in the prefix root and a
 * multi-addon one in an addon inside it, but both are read the same way, and the manifest goes to
 * the same place either way: the mod root, which is what the model already worked out.
 *
 * `folders` are the folders open in the workspace, because the mod root of an unconfigured mod is
 * the prefix root's parent — and a repository that *is* the prefix root has its mod root above
 * everything that is open. A file written there is a file the search never looks at again.
 */
export function adoptionOf(mod: Mod, source: string, folders: readonly string[]): Adoption {
  const fields = modFieldsOf(parseConfig(source), mod.name);
  const modName = modNameOf(fields.name);
  const refusal = adoptionRefusalOf(mod, fields.name, folders);

  return {
    fields,
    modName,
    folders: [],
    files:
      refusal === undefined
        ? [{ path: MANIFEST_FILE, content: manifestOf(fields.name, fields) }]
        : [],
    refusal,
  };
}

/** Why this mod is not one to adopt, or undefined when it is. */
function adoptionRefusalOf(
  mod: Mod,
  adoptedName: string,
  folders: readonly string[],
): string | undefined {
  if (mod.manifest !== undefined) {
    return `${mod.name} is configured already: it has a ${MANIFEST_FILE}.`;
  }

  // The mod is in the list because something under it carries `CfgMods`, but that something is
  // not one of the addons — it sits deeper than one — so no pbo of this mod declares it.
  if (mainAddonOf(mod) === undefined) {
    return (
      `${mod.name} has no main addon: nothing that packs into a pbo here declares the mod in a ` +
      `CfgMods block, so there is nothing to fill a ${MANIFEST_FILE} in from.`
    );
  }

  const nameProblem = modNameProblemOf(adoptedName);
  if (nameProblem !== undefined) {
    return `${mod.name} cannot be configured as ${JSON.stringify(adoptedName)}. ${nameProblem}`;
  }

  // The mod root holds the prefix root rather than being it, so a mod whose prefix root is the
  // open folder itself has nowhere inside the workspace for its manifest to go.
  if (!folders.some((folder) => isWithin(mod.root, folder))) {
    return (
      `${mod.name} is the folder that is open, and a ${MANIFEST_FILE} belongs beside its prefix ` +
      'root rather than inside it — so it would have to go above everything open here, where ' +
      `nothing would ever find it. Open the folder holding ${mod.name} and write it there.`
    );
  }

  return undefined;
}

/**
 * What the config already says about the mod. `CfgMods` is where a mod describes itself, and its
 * addon is the second place to ask: `author` and `version` in a `CfgPatches` class are an Arma
 * habit that plenty of DayZ configs keep.
 *
 * The name is taken from `dir` rather than from `name`, and this is the one field where the two
 * differ on purpose. A manifest's name is not a title: it is the name the mod is linked and loaded
 * under, which is what `dir` holds. What the config's `name` says — "Foreign Mod", with a space in
 * it — is what the launcher shows, and the launcher reads that out of `mod.cpp`, which the mod
 * already has and adoption does not touch. The prefix root's name is the last resort, for a config
 * that names no `dir` at all.
 */
export function modFieldsOf(config: ConfigCpp, name: string): ModFields {
  return {
    name: config.mod?.dir ?? name,
    description: config.mod?.overview,
    author: config.mod?.author ?? patchFieldOf(config, (patch) => patch.author),
    version: config.mod?.version ?? patchFieldOf(config, (patch) => patch.version),
  };
}

/** The first of the addon's `CfgPatches` classes to answer; a config declares more than one. */
function patchFieldOf(
  config: ConfigCpp,
  field: (patch: PatchClass) => string | undefined,
): string | undefined {
  return config.patches.map(field).find((value) => value !== undefined);
}

/**
 * A new addon of a mod that already has one. Only a mod already laid out as several addons takes
 * another: in a single-addon mod the `config.cpp` sits in the prefix root and packs everything
 * under it, so a new addon there would be a folder its parent is already packing — which is a
 * layout to be moved into rather than added to.
 */
export function addonPlanOf(mod: Mod, name: string): AddonPlan {
  const refusal = addonRefusalOf(mod, name);
  if (refusal !== undefined) {
    return { folders: [], files: [], requires: undefined, refusal, warning: undefined };
  }

  const within = withinOf(mod);
  const folder = within === '' ? name : `${within}/${name}`;
  const patch = `${mod.name}_${name}`;
  const main = mainAddonOf(mod);
  const into = main?.patches[0];

  return {
    folders: [folder],
    files: [{ path: `${folder}/${CONFIG_FILE}`, content: addonConfigOf(patch) }],
    requires:
      main === undefined || into === undefined
        ? undefined
        : { config: main.config, patch: into, required: patch },
    refusal: undefined,
    // A mod whose main addon declares no `CfgPatches` class has nothing to hang the new addon off,
    // and the addon is still worth making — so it is made, and this is said.
    warning:
      main === undefined || into === undefined
        ? `Nothing in ${mod.name} requires ${patch}: its main addon declares no CfgPatches class ` +
          'to write the name into, so the engine is free to load the addons in any order.'
        : undefined,
  };
}

/** The main addon's `config.cpp` with the new addon written into it, where it can be written. */
export function requiringAddon(source: string, requirement: AddonRequirement): string | undefined {
  return withRequiredAddon(source, requirement.patch, requirement.required);
}

/**
 * Why this mod takes no addon at all, whatever it would be called, or undefined when it does. It
 * is asked before the name is: a developer about to be told that this mod is one pbo should not
 * have to think of a name for an addon first.
 */
export function addonsRefusalOf(mod: Mod): string | undefined {
  const nameProblem = modNameProblemOf(mod.name);
  if (nameProblem !== undefined) {
    return `${mod.name || 'This mod'} cannot take an addon. ${nameProblem}`;
  }

  if (mod.prefixRoot === undefined) {
    return `${mod.name} has no prefix root to put an addon in.`;
  }

  if (mod.layout === 'single') {
    return (
      `${mod.name} is one addon already: its ${CONFIG_FILE} sits in the prefix root, so the whole ` +
      'mod packs into one pbo. Move that file and what belongs with it into a folder inside the ' +
      'prefix root to make the mod several addons, and then add another.'
    );
  }

  return undefined;
}

/** Why this mod takes no addon of this name, or undefined when it does. */
export function addonRefusalOf(mod: Mod, name: string): string | undefined {
  return (
    addonsRefusalOf(mod) ??
    addonNameProblemOf(name) ??
    (mod.addons.some((addon) => addon.name.toLowerCase() === name.toLowerCase())
      ? `${mod.name} already has an addon called ${name}.`
      : undefined)
  );
}

/** The prefix root under the mod root, which is what the plan's paths are counted from. */
function withinOf(mod: Mod): string {
  const prefixRoot = mod.prefixRoot ?? '';

  return prefixRoot.startsWith(`${mod.root}/`) ? prefixRoot.slice(mod.root.length + 1) : '';
}

/** The folder a mod keeps its scripts in, and the addon a multi-addon mod declares itself in. */
const SCRIPTS = 'Scripts';

const INPUTS = 'Inputs.xml';

const MOD_CPP = 'mod.cpp';

/** The file the dev server is started with, which a launch looks for in the mod's root. */
const SERVER_CONFIG = 'server.cfg';

const STRINGTABLE_FILE = 'stringtable.csv';

const GITIGNORE_FILE = '.gitignore';

/** What holds an otherwise empty folder in git, since git keeps files rather than folders. */
const KEEP_FILE = '.gitkeep';

const MISSIONS = 'Missions';

const PROFILES = 'Profiles';

/** The layer every launch lays down, and the one only a development launch does. */
const GLOBAL = 'Global';

const DEV = 'Dev';

/** Where the built mod goes, which is what `modsDirectory` is set to. */
const BUILT = 'Addons';

/** What a mod that has never said which version it is starts at. */
const VERSION = '0.1.0';

/** One engine script module: how `CfgMods` attaches it, and what the folder is called on disk. */
interface ScriptModule {
  /** The folder under `Scripts`, which is what the module's `files[]` points at. */
  readonly folder: string;
  /** The class under `defs` the engine reads that path out of. */
  readonly declaration: string;
  /** What is compiled there, for the line at the top of the file that goes in it. */
  readonly what: string;
}

/**
 * The four modules a mod gets, in the order the engine compiles them. `2_GameLib` is not among
 * them: it is the engine's own library layer, and a mod with no use for it is every mod.
 */
const MODULES: readonly ScriptModule[] = [
  { folder: '1_Core', declaration: 'engineScriptModule', what: 'the engine layer' },
  { folder: '3_Game', declaration: 'gameScriptModule', what: 'the game layer' },
  { folder: '4_World', declaration: 'worldScriptModule', what: 'the world layer' },
  { folder: '5_Mission', declaration: 'missionScriptModule', what: 'the mission layer' },
];

/**
 * What the mod says about itself. `modsDirectory` is filled in, because a mod that cannot be built
 * until a field is found is not a mod that was started for the developer; a field nobody has
 * answered for is left as a comment, which is both the hint and the place to write the answer.
 *
 * `name` is the mod's one name — what it is called, linked and loaded as; `mod` is the name the
 * model already has it under, which is what the path in the comment is worked out from. For a new
 * mod the two are the same, and for an adopted one they are whatever its config said.
 */
function manifestOf(mod: string, fields: ModFields): string {
  return `{
  // The mod's name: what the panel shows, what it goes onto the work drive as (P:\\${fields.name})
  // and what it is built into (@${fields.name}). The folder's own name when left out.
  "name": ${quoted(fields.name)},
  "version": ${quoted(fields.version ?? VERSION)},
${fieldLine('description', fields.description, 'What the mod does, in a sentence.')}
${fieldLine('author', fields.author, 'Who made it.')}

  "launch": {
    // Where the built mod goes, counted from this file: ${BUILT}\\@${mod}.
    "modsDirectory": "${BUILT}",
    // What every target loads, in load order: nothing is added to this list on the way to the
    // game, so the mod itself is named here, and any mod it needs goes in front of it.
    "mods": [${quoted(`@${fields.name}`)}],
    "targets": [
      {
        // The client alone, which loads the vanilla offline mission of the map: a mod is seen
        // loaded without a mission of your own having been written first.
        "name": "Client",
        "map": "ChernarusPlus",
        "run": "client"
      }
    ]
  }
}
`;
}

/**
 * What a mod made in a workspace says about itself, and no launch block: the `workspace.enf` above
 * owns the launch of every mod under it, so one written here would be ignored whole — and a block
 * that is there and does nothing is a block somebody edits and then wonders about. The version is
 * the last field so that nothing after it needs a comma; the two unanswered ones sit above it.
 */
function workspaceModManifestOf(name: string): string {
  return `{
  // The mod's name: what the panel shows, what it goes onto the work drive as (P:\\${name})
  // and what it is built into (@${name}). The folder's own name when left out.
  "name": ${quoted(name)},
  // "description": "What the mod does, in a sentence.",
  // "author": "Who made it.",
  "version": ${quoted(VERSION)}

  // No "launch" here: the ${WORKSPACE_FILE} above owns the launch of every mod under it, and a
  // block written here would be ignored whole rather than merged into that one. This mod loads
  // because it is named in that file's "mods".
}
`;
}

/**
 * What a new workspace says: nothing about any one mod, and the launch block of all of them. The
 * mods list starts empty and grows one name per mod made in the workspace, and the one target
 * names no mod, so that it takes its profile and mission from whichever mod comes first.
 */
const WORKSPACE_MANIFEST = `{
  // Folders this workspace does not see, each written relative to this file: nothing under one is
  // listed, built, linked or launched from this window.
  // "ignore": ["Folder"],

  // The launch block of every mod under this folder. A "launch" written in one of their mod.enf
  // is ignored whole rather than merged into this one.
  "launch": {
    // Where the built mods go, counted from this file: ${BUILT}\\@<Mod>.
    "modsDirectory": "${BUILT}",
    // What every target loads, in load order: nothing is added to this list on the way to the
    // game. Create Mod names each mod it makes here; a mod one needs goes in front of it.
    "mods": [],
    "targets": [
      {
        // The client alone, which loads the vanilla offline mission of the map. A target that
        // names no "mod" takes its profile and mission from the first mod of the workspace.
        "name": "Client",
        "map": "ChernarusPlus",
        "run": "client"
      }
    ]
  }
}
`;

/** The field where something answered for it, and the hint that asks for it where nothing did. */
function fieldLine(field: string, value: string | undefined, hint: string): string {
  return value === undefined ? `  // "${field}": "${hint}",` : `  "${field}": ${quoted(value)},`;
}

/**
 * A value the way JSON writes it, quotes and all. What a `config.cpp` holds is not what JSON
 * takes: an overview with a quote in it — and the config syntax for one is `""` — would end the
 * string early and leave a manifest nothing can read.
 */
function quoted(value: string): string {
  return JSON.stringify(value);
}

/**
 * The one file a mod cannot load without, with every path in it worked out from the name.
 *
 * `CfgPatches` registers the pbo and orders it after the vanilla scripts; `CfgMods` declares the
 * mod itself — the folder it loads from, and the four script modules the engine compiles the mod's
 * scripts into. `inputs` is what makes `Inputs.xml` more than a file in a folder; a
 * `stringtable.csv` needs no declaration at all, because the engine reads one out of the root of
 * every pbo it loads.
 */
function configOf(name: string, layout: Layout): string {
  const patch = layout === 'single' ? name : `${name}_${SCRIPTS}`;
  const packed =
    layout === 'single'
      ? `// One pbo for the whole mod: this file sits in the prefix root, so everything under
// P:\\${name} is packed into ${name}.pbo with the prefix "${name}".`
      : `// One pbo per addon: this file sits in ${SCRIPTS}, so what is packed into ${SCRIPTS}.pbo is that
// folder alone, with the prefix "${name}\\${SCRIPTS}". The mod is declared here because this is its
// main addon — the one carrying CfgMods.`;

  return `${packed}
class CfgPatches
{
	class ${patch}
	{
		units[] = {};
		weapons[] = {};
		requiredVersion = 0.1;
		requiredAddons[] = { "DZ_Scripts" };
	};
};

class CfgMods
{
	class ${name}
	{
		type = "mod";
		dir = "${name}";
		name = "${name}";
		inputs = "${name}/${SCRIPTS}/${INPUTS}";
		dependencies[] = { "Game", "World", "Mission" };

		class defs
		{
${MODULES.map((module) => declarationOf(name, module)).join('\n\n')}
		};
	};
};
`;
}

/** One script module attached to the engine's, by the path its scripts sit at in the pbo. */
function declarationOf(name: string, module: ScriptModule): string {
  return `			class ${module.declaration}
			{
				value = "";
				files[] = { "${name}/${SCRIPTS}/${module.folder}" };
			};`;
}

/** An addon that is not the main one: it packs into a pbo and declares nothing about the mod. */
function addonConfigOf(patch: string): string {
  return `class CfgPatches
{
	class ${patch}
	{
		units[] = {};
		weapons[] = {};
		requiredVersion = 0.1;
		requiredAddons[] = { "DZ_Data" };
	};
};
`;
}

/**
 * The dev server's configuration: DayZ Server's own `serverDZ.cfg`, with what a local launch needs
 * changed. A Debug launch runs the diag build off the sources rather than off signed pbo, so the
 * server lets file patching in and checks no signatures; the clock stands at noon, because a dev
 * server that comes up at night is one nobody can see anything on.
 *
 * The mission at the bottom is never the one loaded: a launch names the mission it laid down on
 * the command line, which wins over it. It is there because a server without it will not start.
 */
function serverConfigOf(title: string): string {
  return `// The dev server a launch puts up for a target that runs a server. Not packed: it sits beside
// the mod, and a launch passes it as -config.
hostname = "${title} dev server";
password = "";
passwordAdmin = "";

enableWhitelist = 0;
maxPlayers = 10;

// Debug launches run the sources rather than signed pbo, so nothing is checked for a signature,
// and clients that patch files in are let in.
verifySignatures = 0;
allowFilePatching = 1;
forceSameBuild = 1;

disableVoN = 0;
vonCodecQuality = 20;
disable3rdPerson = 0;
disableCrosshair = 0;

// Noon, passing slowly, and not kept between launches.
serverTime = "2020/7/1/12/00";
serverTimeAcceleration = 0.1;
serverNightTimeAcceleration = 1;
serverTimePersistent = 0;

guaranteedUpdates = 1;
loginQueueConcurrentPlayers = 5;
loginQueueMaxPlayers = 500;
instanceId = 1;
storageAutoFix = 1;

class Missions
{
	class DayZ
	{
		// Never loaded: the launch names its own mission with -mission, which wins over this.
		template = "dayzOffline.chernarusplus";
	};
};
`;
}

/** What the launcher shows. Not packed: the build copies it into the built mod itself. */
function modCppOf(name: string): string {
  return `// What the DayZ launcher shows about this mod. It is not packed into the pbo — a builder packs
// the addon, and this sits above it — so the build copies it into <ModsDirectory>\\@${name}.
name = "${name}";
picture = "";
logo = "";
logoSmall = "";
logoOver = "";
tooltip = "${name}";
overview = "";
action = "";
author = "";
version = "0.1.0";
`;
}

function moduleFileOf(name: string, module: ScriptModule): string {
  return `// ${name} in ${module.what}. Every .c file in this folder is compiled into the engine's
// ${module.declaration}, which is what CfgMods attaches ${name}/${SCRIPTS}/${module.folder} to.
`;
}

/** The keys the mod binds, which `CfgMods` points the engine at through its `inputs`. */
const INPUTS_XML = `<?xml version="1.0" encoding="UTF-8" standalone="yes" ?>
<modded_inputs>
	<inputs>
		<actions>
			<!-- Actions go here -->
		</actions>
	</inputs>

	<preset>
		<!-- Presets for the actions go here -->
	</preset>
</modded_inputs>
`;

/** The columns of a stringtable, in the order the game's own tables have them. */
const STRINGTABLE_LANGUAGES = [
  'original', 'english', 'czech', 'german', 'russian', 'polish', 'hungarian', 'italian', 'spanish',
  'french', 'chinese', 'japanese', 'portuguese', 'chinesesimp',
];

/**
 * A stringtable with the mod's name as its one string. It goes in the root of the main addon,
 * because the root of the pbo is where the engine reads one from. Every field is quoted and comma
 * separated, the way the game's own tables are: a table of tabs, or one with a header and no rows,
 * is what the server reports as `Invalid StringTable`.
 */
function stringtableOf(name: string): string {
  const row = (fields: readonly string[]): string =>
    fields.map((field) => `"${field.replace(/"/g, '""')}"`).join(',') + ',\r\n';
  const translated = STRINGTABLE_LANGUAGES.map((language) =>
    language === 'original' || language === 'english' ? name : '');

  return row(['Language', ...STRINGTABLE_LANGUAGES]) + row([`STR_${name}_Name`, ...translated]);
}

/** What never belongs in a repository: what the build makes, what the game writes, and the key. */
const BUILD_IGNORED = `# What the build makes.
/${BUILT}/
*.pbo
*.bisign

# The key that signs the pbo. The public one is meant to be shared; this one never is.
*.biprivatekey

# What the game and the tools write while they run.
*.RPT
*.log
*.ADM
*.mdmp
*.DayZProfile
texHeaders.bin
dayz.bin
`;

/** A mod's own has its profiles to keep out as well; a workspace keeps none of its own. */
const GITIGNORE = `${BUILD_IGNORED}
# What the game keeps about whoever played with this profile.
${PROFILES}/**/Users/*
`;
