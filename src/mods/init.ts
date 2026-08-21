/**
 * Starting a mod, and adding an addon to one.
 *
 * A mod is a folder with `mod.enf`, holding a prefix root of the mod's own name, and inside that
 * the addons. Getting one of those wrong — a `dir` that does not match the folder, a script module
 * path in the wrong case, a `CfgPatches` nobody requires — is the sort of mistake that shows up as
 * a mod which packs cleanly and then does nothing in the game, so none of it is typed by hand
 * here: every name in every file is worked out from the one name the developer gave.
 *
 * The whole of it comes out as a plan — folders to make, files to write, and for an addon the one
 * edit that keeps it from being lost — so that what a new mod is made of can be compared whole in
 * a test instead of by watching what was written. Nothing here goes near a disk.
 *
 * Paths are `/` separated and counted from the mod root, which is the folder `mod.enf` goes in.
 */

import { withRequiredAddon } from './config';
import { CONFIG_FILE, type Layout, MANIFEST_FILE, type Mod } from './model';

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

/** One name to write into one `CfgPatches` class of one file. */
export interface AddonRequirement {
  /** The main addon's `config.cpp`, as the model has it. */
  readonly config: string;
  /** The class in it that the name goes into. */
  readonly patch: string;
  /** The new addon's own `CfgPatches` name. */
  readonly required: string;
}

/**
 * Why the name will not do, or undefined when it will. A mod's name is its folder, the `dir` of
 * its `CfgMods`, the `P:\<Name>` it is linked as, the `@<Name>` it is loaded as and the class its
 * `CfgPatches` goes by — so it has to be a name all five of those take, which is the narrowest of
 * them: a class name.
 */
export function modNameProblemOf(name: string): string | undefined {
  if (name.trim() === '') {
    return 'A mod needs a name: it is the folder it is linked and loaded under.';
  }

  return NAME.test(name) ? undefined : named('A mod');
}

/** The same, for the folder an addon is packed out of, which is a class name too. */
export function addonNameProblemOf(name: string): string | undefined {
  if (name.trim() === '') {
    return 'An addon needs a name: it is the folder it is packed out of, and its pbo.';
  }

  return NAME.test(name) ? undefined : named('An addon');
}

function named(what: string): string {
  return (
    `${what} is named by a class as well as by a folder: letters, digits and underscores, ` +
    'starting with a letter.'
  );
}

/** What a class in a `config.cpp` can be called, which a folder can always be called too. */
const NAME = /^[A-Za-z_][A-Za-z0-9_]*$/;

/**
 * Everything a new mod is made of. The layout is the one thing asked besides the name, because it
 * is the one thing that cannot be changed later without rewriting every path in `CfgMods`: a mod
 * of one pbo keeps its `config.cpp` in the prefix root, and a mod of several keeps one per addon.
 *
 * The script module paths come out the same either way — `<Name>/Scripts/<module>` — because in a
 * single-addon mod the prefix root is the addon, and in a multi-addon one the addon is `Scripts`
 * inside it. Which is what lets a mod be split up later by moving files rather than editing paths.
 */
export function initPlanOf(name: string, layout: Layout): InitPlan {
  const main = layout === 'single' ? name : `${name}/${SCRIPTS}`;

  return {
    folders: [
      name,
      `${name}/${SCRIPTS}`,
      ...MODULES.map((module) => `${name}/${SCRIPTS}/${module.folder}`),
      // What never reaches a pbo: the layers a launch lays a profile and a mission down from, and
      // the folder the built mod is written into.
      MISSIONS,
      `${MISSIONS}/${GLOBAL}`,
      PROFILES,
      `${PROFILES}/${GLOBAL}`,
      `${PROFILES}/${DEV}`,
      BUILT,
    ],
    files: [
      { path: MANIFEST_FILE, content: manifestOf(name) },
      { path: GITIGNORE_FILE, content: GITIGNORE },
      { path: `${main}/${CONFIG_FILE}`, content: configOf(name, layout) },
      { path: `${name}/${MOD_CPP}`, content: modCppOf(name) },
      { path: `${main}/${STRINGTABLE_FILE}`, content: STRINGTABLE },
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
    ],
  };
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
  const main = mod.addons.find((addon) => addon.main);
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
 * until a field is found is not a mod that was started for the developer; the fields left as
 * comments are the ones only the developer can answer.
 */
function manifestOf(name: string): string {
  return `{
  // What the panel and the launcher call this mod; the prefix root's name when left out.
  "name": "${name}",
  "version": "0.1.0",
  // "description": "What the mod does, in a sentence.",
  // "author": "Who made it.",

  "launch": {
    // Where the built mod goes, counted from this file: ${BUILT}\\@${name}.
    "modsDirectory": "${BUILT}",
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

/**
 * The header row of a stringtable, which is the whole of an empty one. It goes in the root of the
 * main addon because the root of the pbo is where the engine reads one from, and it is tab
 * separated because that is what the engine parses.
 */
const STRINGTABLE =
  'Language\toriginal\tenglish\tczech\tgerman\trussian\tpolish\thungarian\titalian\tspanish\t' +
  'french\tchinese\tjapanese\tportuguese\tchinesesimp\n';

/** What never belongs in a repository: what the build makes, what the game writes, and the key. */
const GITIGNORE = `# What the build makes.
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

# What the game keeps about whoever played with this profile.
${PROFILES}/**/Users/*
`;
