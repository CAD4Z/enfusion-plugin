import assert from 'node:assert/strict';
import { test } from 'node:test';
import { type ModDefs, parseConfig } from '../../src/mods/config';
import {
  type AddonPlan,
  type Adoption,
  type InitPlace,
  type InitPlan,
  type Placed,
  type Surroundings,
  addonNameProblemOf,
  addonPlanOf,
  adoptionOf,
  initPlanOf,
  mergedLinesOf,
  modFieldsOf,
  modFolderRefusalOf,
  modPlacementOf,
  plannedDefsOf,
  requiringAddon,
  withWorkspaceMod,
  workspaceFolderRefusalOf,
  workspacePlanOf,
} from '../../src/mods/init';
import { type ModName, modNameOf, modNameProblemOf } from '../../src/mods/modName';
import { type Mod, modsFromScan } from '../../src/mods/model';
import { projectFileOf } from '../../src/mods/workbench';

/** A mod made on its own, on a machine whose work drive is the usual one. */
const ALONE: InitPlace = { drive: 'P:', inWorkspace: false };

/** A mod made in a workspace, which owns its launch and its Workbench project. */
const IN_WORKSPACE: InitPlace = { drive: 'P:', inWorkspace: true };

/**
 * The whole of a new single-addon mod, compared entire. What matters about an initialisation is
 * what is on the disk afterwards — every folder, every file and every line in it — rather than
 * which of them was written first, so this is the shape the test takes.
 */
test('a single-addon mod comes out whole, with every name in it worked out from the one given', () => {
  assert.deepEqual(initPlanOf(name('MyMod'), 'single', ALONE), {
    folders: [
      'MyMod',
      'MyMod/Scripts',
      'MyMod/Scripts/1_Core',
      'MyMod/Scripts/3_Game',
      'MyMod/Scripts/4_World',
      'MyMod/Scripts/5_Mission',
      'Missions',
      'Missions/Global',
      'Profiles',
      'Profiles/Global',
      'Profiles/Dev',
      'Addons',
      'Workbench',
    ],
    files: [
      {
        path: 'mod.enf',
        content: `{
  // The mod's name: what the panel shows, what it goes onto the work drive as (P:\\MyMod)
  // and what it is built into (@MyMod). The folder's own name when left out.
  "name": "MyMod",
  "version": "0.1.0",
  // "description": "What the mod does, in a sentence.",
  // "author": "Who made it.",

  "launch": {
    // Where the built mod goes, counted from this file: Addons\\@MyMod.
    "modsDirectory": "Addons",
    // What every target loads, in load order: nothing is added to this list on the way to the
    // game, so the mod itself is named here, and any mod it needs goes in front of it.
    "mods": ["@MyMod"],
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
`,
      },
      {
        path: '.gitignore',
        content: `# What the build makes.
/Addons/
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
Profiles/**/Users/*
`,
        merge: 'lines',
      },
      {
        path: 'MyMod/config.cpp',
        content: `// One pbo for the whole mod: this file sits in the prefix root, so everything under
// P:\\MyMod is packed into MyMod.pbo with the prefix "MyMod".
class CfgPatches
{
	class MyMod
	{
		units[] = {};
		weapons[] = {};
		requiredVersion = 0.1;
		requiredAddons[] = { "DZ_Scripts" };
	};
};

class CfgMods
{
	class MyMod
	{
		type = "mod";
		dir = "MyMod";
		name = "MyMod";
		inputs = "MyMod/Scripts/Inputs.xml";
		dependencies[] = { "Game", "World", "Mission" };

		class defs
		{
			class engineScriptModule
			{
				value = "";
				files[] = { "MyMod/Scripts/1_Core" };
			};

			class gameScriptModule
			{
				value = "";
				files[] = { "MyMod/Scripts/3_Game" };
			};

			class worldScriptModule
			{
				value = "";
				files[] = { "MyMod/Scripts/4_World" };
			};

			class missionScriptModule
			{
				value = "";
				files[] = { "MyMod/Scripts/5_Mission" };
			};
		};
	};
};
`,
      },
      {
        path: 'MyMod/mod.cpp',
        content: `// What the DayZ launcher shows about this mod. It is not packed into the pbo — a builder packs
// the addon, and this sits above it — so the build copies it into <ModsDirectory>\\@MyMod.
name = "MyMod";
picture = "";
logo = "";
logoSmall = "";
logoOver = "";
tooltip = "MyMod";
overview = "";
action = "";
author = "";
version = "0.1.0";
`,
      },
      {
        path: 'server.cfg',
        content: `// The dev server a launch puts up for a target that runs a server. Not packed: it sits beside
// the mod, and a launch passes it as -config.
hostname = "MyMod dev server";
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
`,
        merge: 'keep',
      },
      {
        path: 'MyMod/stringtable.csv',
        content:
          '"Language","original","english","czech","german","russian","polish","hungarian",' +
          '"italian","spanish","french","chinese","japanese","portuguese","chinesesimp",\r\n' +
          '"STR_MyMod_Name","MyMod","MyMod","","","","","","","","","","","","",\r\n',
      },
      {
        path: 'MyMod/Scripts/Inputs.xml',
        content: `<?xml version="1.0" encoding="UTF-8" standalone="yes" ?>
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
`,
      },
      {
        path: 'MyMod/Scripts/1_Core/MyMod.c',
        content: `// MyMod in the engine layer. Every .c file in this folder is compiled into the engine's
// engineScriptModule, which is what CfgMods attaches MyMod/Scripts/1_Core to.
`,
      },
      {
        path: 'MyMod/Scripts/3_Game/MyMod.c',
        content: `// MyMod in the game layer. Every .c file in this folder is compiled into the engine's
// gameScriptModule, which is what CfgMods attaches MyMod/Scripts/3_Game to.
`,
      },
      {
        path: 'MyMod/Scripts/4_World/MyMod.c',
        content: `// MyMod in the world layer. Every .c file in this folder is compiled into the engine's
// worldScriptModule, which is what CfgMods attaches MyMod/Scripts/4_World to.
`,
      },
      {
        path: 'MyMod/Scripts/5_Mission/MyMod.c',
        content: `// MyMod in the mission layer. Every .c file in this folder is compiled into the engine's
// missionScriptModule, which is what CfgMods attaches MyMod/Scripts/5_Mission to.
`,
      },
      { path: 'Missions/Global/.gitkeep', content: '' },
      { path: 'Profiles/Global/.gitkeep', content: '' },
      { path: 'Profiles/Dev/.gitkeep', content: '' },
      // The project lists the folders the config above attaches; what a project looks like is
      // the Workbench tests' to pin, so here it is what goes into one that is compared.
      // One somebody made already is kept rather than refused over: the one written here is a start.
      {
        path: 'Workbench/dayz.gproj',
        content: projectFileOf('P:', 'MyMod', [scriptsOf('MyMod')]),
        merge: 'keep',
      },
    ],
  } satisfies InitPlan);
});

/** The folders a mod made here attaches, which are the same in either layout. */
function scriptsOf(mod: string): ModDefs {
  return {
    scripts: {
      core: [`${mod}/Scripts/1_Core`],
      gameLib: [],
      game: [`${mod}/Scripts/3_Game`],
      world: [`${mod}/Scripts/4_World`],
      mission: [`${mod}/Scripts/5_Mission`],
    },
    imageSets: [],
    widgetStyles: [],
  };
}

/**
 * The multi-addon layout differs in one thing only: which folder the `config.cpp` and the
 * stringtable sit in. The paths `CfgMods` carries are the same either way, which is what lets a
 * mod be split up later by moving files rather than by editing every path in the config.
 */
test('a multi-addon mod puts the same files in Scripts, and points CfgMods at the same paths', () => {
  const plan = initPlanOf(name('MyMod'), 'multi', ALONE);
  const single = initPlanOf(name('MyMod'), 'single', ALONE);

  assert.deepEqual(
    plan.files.map((file) => file.path),
    [
      'mod.enf',
      '.gitignore',
      'MyMod/Scripts/config.cpp',
      'MyMod/mod.cpp',
      'server.cfg',
      'MyMod/Scripts/stringtable.csv',
      'MyMod/Scripts/Inputs.xml',
      'MyMod/Scripts/1_Core/MyMod.c',
      'MyMod/Scripts/3_Game/MyMod.c',
      'MyMod/Scripts/4_World/MyMod.c',
      'MyMod/Scripts/5_Mission/MyMod.c',
      'Missions/Global/.gitkeep',
      'Profiles/Global/.gitkeep',
      'Profiles/Dev/.gitkeep',
      'Workbench/dayz.gproj',
    ],
  );
  assert.deepEqual(plan.folders, single.folders);
  // The script folders are the same in both layouts, and so is the project listing them.
  assert.equal(contentOf(plan, 'Workbench/dayz.gproj'), contentOf(single, 'Workbench/dayz.gproj'));
  assert.deepEqual(plannedDefsOf(plan), scriptsOf('MyMod'));

  const config = contentOf(plan, 'MyMod/Scripts/config.cpp');
  assert.match(config, /class CfgPatches\s*\{\s*class MyMod_Scripts\b/);
  assert.match(config, /dir = "MyMod";/);
  for (const module of ['1_Core', '3_Game', '4_World', '5_Mission']) {
    assert.ok(
      config.includes(`files[] = { "MyMod/Scripts/${module}" };`),
      `${module} is attached at the same path in both layouts`,
    );
  }
});

/** The name is one the developer typed, and it becomes both classes and folders. */
test('a name that cannot be both class and folder is refused, and every other one is taken', () => {
  assert.equal(modNameProblemOf('MyMod'), undefined);
  assert.equal(modNameProblemOf('My_Mod_2'), undefined);

  assert.match(modNameProblemOf('') ?? '', /needs a name/);
  assert.match(modNameProblemOf('   ') ?? '', /needs a name/);
  assert.match(modNameProblemOf('My Mod') ?? '', /letters, digits and underscores/);
  assert.match(modNameProblemOf('2Mods') ?? '', /starting with a letter/);
  assert.match(modNameProblemOf('My-Mod') ?? '', /letters, digits and underscores/);
  assert.match(addonNameProblemOf('CON') ?? '', /reserved Windows device name/);
});

test('an addon is a folder in the prefix root, and a name in the main addon of the mod', () => {
  const plan = addonPlanOf(multiAddonMod(), 'Data');

  assert.deepEqual(plan, {
    folders: ['MyMod/Data'],
    files: [
      {
        path: 'MyMod/Data/config.cpp',
        content: `class CfgPatches
{
	class MyMod_Data
	{
		units[] = {};
		weapons[] = {};
		requiredVersion = 0.1;
		requiredAddons[] = { "DZ_Data" };
	};
};
`,
      },
    ],
    requires: {
      config: '/w/MyMod/MyMod/Scripts/config.cpp',
      patch: 'MyMod_Scripts',
      required: 'MyMod_Data',
    },
    refusal: undefined,
    warning: undefined,
  } satisfies AddonPlan);
});

/** The edit itself, which is what keeps the new addon from being loaded in whatever order. */
test('the name is written into the requiredAddons of the main addon', () => {
  const plan = addonPlanOf(multiAddonMod(), 'Data');
  const requirement = plan.requires;
  assert.ok(requirement);

  assert.equal(
    requiringAddon(mainConfig(), requirement),
    `class CfgPatches
{
	class MyMod_Scripts
	{
		requiredAddons[] = { "DZ_Scripts", "MyMod_Data" };
	};
};

class CfgMods
{
	class MyMod
	{
		dir = "MyMod";
	};
};
`,
  );
});

test('a single-addon mod is told what it would take to have addons, and nothing is planned', () => {
  const plan = addonPlanOf(singleAddonMod(), 'Data');

  assert.deepEqual(plan.folders, []);
  assert.deepEqual(plan.files, []);
  assert.equal(plan.requires, undefined);
  assert.match(plan.refusal ?? '', /is one addon already/);
});

test('an addon of a name the mod already has is refused rather than written over', () => {
  const plan = addonPlanOf(multiAddonMod(), 'scripts');

  assert.deepEqual(plan.files, []);
  assert.match(plan.refusal ?? '', /already has an addon called scripts/);
});

/** A mod nothing declares `CfgMods` in still takes an addon; what it does not take is an edit. */
test('with no main addon to require it, the addon is still made and the silence is reported', () => {
  const mod = modOf({
    manifests: ['/w/MyMod/mod.enf'],
    configs: [
      { path: '/w/MyMod/MyMod/Scripts/config.cpp', source: 'class CfgMods { class MyMod { dir = "MyMod"; }; };' },
    ],
  });

  const plan = addonPlanOf(mod, 'Data');

  assert.deepEqual(plan.folders, ['MyMod/Data']);
  assert.equal(plan.requires, undefined);
  assert.equal(plan.refusal, undefined);
  assert.match(plan.warning ?? '', /Nothing in MyMod requires MyMod_Data/);
});

/**
 * The one thing an unconfigured mod is asked nothing for. What a `config.cpp` already says about
 * the mod is exactly what a `mod.enf` would otherwise be typed out with, so this is read whole:
 * every field, and where each of them came from.
 */
test('the fields of a mod.enf are read off the config that already carries them', () => {
  assert.deepEqual(modFieldsOf(parseConfig(foreignConfig()), 'Foreign'), {
    name: 'Foreign',
    description: 'What somebody else made it do.',
    author: 'somebody',
    version: '1.4',
  });
});

/** A mod that says nothing about itself still has an addon and a folder that do. */
test('what the mod leaves out is taken from its addon, and its name from the prefix root', () => {
  const config = parseConfig(`
class CfgPatches
{
	class Foreign_Scripts
	{
		requiredAddons[] = { "DZ_Scripts" };
		author = "somebody";
		version = "1.4";
	};
};

class CfgMods { class Foreign { dir = "Foreign"; name = ""; }; };
`);

  assert.deepEqual(modFieldsOf(config, 'Foreign'), {
    name: 'Foreign',
    description: undefined,
    author: 'somebody',
    version: '1.4',
  });
});

test('an unconfigured mod is adopted by the one file it is missing, and nothing else', () => {
  const mod = foreignMod('/w/Foreign/Foreign/config.cpp');

  assert.deepEqual(adoptionOf(mod, foreignConfig(), ['/w']), {
    fields: {
      name: 'Foreign',
      description: 'What somebody else made it do.',
      author: 'somebody',
      version: '1.4',
    },
    modName: name('Foreign'),
    folders: [],
    files: [{ path: 'mod.enf', content: adoptedManifest() }],
    refusal: undefined,
  } satisfies Adoption);
});

test('adoption refuses an unsafe dir instead of writing or linking it', () => {
  const mod = foreignMod('/w/Foreign/Foreign/config.cpp');
  const source = foreignConfig().replace('dir = "Foreign";', 'dir = "../Victim";');
  const adoption = adoptionOf(mod, source, ['/w']);

  assert.equal(adoption.fields.name, '../Victim');
  assert.equal(adoption.modName, undefined);
  assert.deepEqual(adoption.files, []);
  assert.match(adoption.refusal ?? '', /cannot be configured/);
});

/**
 * The layout is read off the tree rather than declared, and adoption asks nothing of it: the same
 * config in the addon of a multi-addon mod fills in the same file, in the same place — the mod
 * root, which is the prefix root's parent when no `mod.enf` marks it.
 */
test('a multi-addon mod is adopted the same way a single-addon one is', () => {
  const mod = foreignMod('/w/Foreign/Foreign/Scripts/config.cpp');

  assert.equal(mod.layout, 'multi');
  assert.equal(mod.root, '/w/Foreign');
  assert.deepEqual(adoptionOf(mod, foreignConfig(), ['/w']).files, [
    { path: 'mod.enf', content: adoptedManifest() },
  ]);
});

/** A config that says nothing gets the same file as one that says everything, filled in less. */
test('a mod with nothing to say about itself is still adopted, under the name it is linked as', () => {
  const mod = foreignMod('/w/Foreign/Foreign/config.cpp');
  const adoption = adoptionOf(mod, 'class CfgMods { class Foreign { dir = "Foreign"; }; };', ['/w']);

  assert.deepEqual(adoption.fields, {
    name: 'Foreign',
    description: undefined,
    author: undefined,
    version: undefined,
  });
  assert.equal(
    contentOf(adoption, 'mod.enf'),
    contentOf(initPlanOf(name('Foreign'), 'single', ALONE), 'mod.enf'),
  );
});

test('a mod that has a mod.enf already is refused rather than written over', () => {
  const adoption = adoptionOf(singleAddonMod(), mainConfig(), ['/w']);

  assert.deepEqual(adoption.files, []);
  assert.match(adoption.refusal ?? '', /MyMod is configured already/);
});

/**
 * A repository that is itself the prefix root has its mod root above everything that is open —
 * the mod root holds the prefix root rather than being it — and a `mod.enf` written up there is
 * one the search never looks at again: the mod would stay unconfigured with a file to show for it.
 */
test('a mod whose prefix root is the open folder is refused, not written above the workspace', () => {
  const mod = foreignMod('/w/Foreign/config.cpp');
  const adoption = adoptionOf(mod, foreignConfig(), ['/w/Foreign']);

  assert.equal(mod.root, '/w');
  assert.deepEqual(adoption.files, []);
  assert.match(adoption.refusal ?? '', /Foreign is the folder that is open/);
});

/** Nothing that packs into a pbo declares the mod, so there is nothing to fill a manifest in from. */
test('a mod whose CfgMods sits below its addons is refused, and told what is missing', () => {
  const mod = modOf({
    manifests: [],
    configs: [{ path: '/w/Foreign/Foreign/Scripts/Core/config.cpp', source: foreignConfig() }],
  });

  const adoption = adoptionOf(mod, foreignConfig(), ['/w']);

  assert.deepEqual(mod.addons, []);
  assert.deepEqual(adoption.files, []);
  assert.match(adoption.refusal ?? '', /Foreign has no main addon/);
});

/**
 * A mod made in a workspace is the same mod without what the workspace has for all of them: the
 * launch block, the folder they are built into, and the Workbench project.
 */
test('a mod made in a workspace has no launch block, Addons, server.cfg or project of its own', () => {
  const plan = initPlanOf(name('MyMod'), 'single', IN_WORKSPACE);
  const alone = initPlanOf(name('MyMod'), 'single', ALONE);

  assert.deepEqual(
    plan.folders,
    alone.folders.filter((folder) => folder !== 'Addons' && folder !== 'Workbench'),
  );
  assert.deepEqual(
    plan.files.map((file) => file.path),
    alone.files
      .map((file) => file.path)
      .filter((path) => path !== 'Workbench/dayz.gproj' && path !== 'server.cfg'),
  );
  assert.equal(
    contentOf(plan, 'mod.enf'),
    `{
  // The mod's name: what the panel shows, what it goes onto the work drive as (P:\\MyMod)
  // and what it is built into (@MyMod). The folder's own name when left out.
  "name": "MyMod",
  // "description": "What the mod does, in a sentence.",
  // "author": "Who made it.",
  "version": "0.1.0"

  // No "launch" here: the workspace.enf above owns the launch of every mod under it, and a
  // block written here would be ignored whole rather than merged into that one. This mod loads
  // because it is named in that file's "mods".
}
`,
  );
  // Read as a manifest, it says what the mod is called and nothing about launching.
  assert.deepEqual(plannedDefsOf(plan), scriptsOf('MyMod'));
});

test('a workspace is a workspace.enf, the folder its mods are built into, and their Workbench project', () => {
  const plan = workspacePlanOf('P:', 'Mods');

  assert.deepEqual(plan.folders, ['Addons', 'Workbench']);
  assert.deepEqual(
    plan.files.map((file) => [file.path, file.merge]),
    [
      ['workspace.enf', undefined],
      ['server.cfg', 'keep'],
      ['.gitignore', 'lines'],
      ['Workbench/dayz.gproj', 'keep'],
    ],
  );
  // The dev server every target of the workspace is put up with, looked for beside workspace.enf.
  assert.match(contentOf(plan, 'server.cfg'), /^hostname = "Mods dev server";$/m);
  assert.equal(contentOf(plan, 'Workbench/dayz.gproj'), projectFileOf('P:', 'Mods', []));
  assert.match(contentOf(plan, 'workspace.enf'), /"modsDirectory": "Addons",\n\s*\/\/[^\n]*\n[^\n]*\n\s*"mods": \[\],/);
  // A workspace keeps no profiles of its own, so it has nothing of them to leave out of git.
  assert.doesNotMatch(contentOf(plan, '.gitignore'), /Profiles/);
});

/** The one file a new mod writes into rather than refusing over: a repository has one already. */
test('a .gitignore that is there already gets the lines it lacks at its end, and keeps its own', () => {
  const existing = '# Mine.\nnode_modules/\n*.log\n';

  assert.equal(
    mergedLinesOf(existing, '# What the build makes.\n/Addons/\n*.pbo\n\n*.log\n'),
    '# Mine.\nnode_modules/\n*.log\n\n# What an Enfusion mod builds, writes while it runs, and signs with.\n/Addons/\n*.pbo\n',
  );
});

test('a .gitignore with every line already in it is left exactly as it was, line endings and all', () => {
  const existing = '/Addons/\r\n*.pbo';

  assert.equal(mergedLinesOf(existing, '/Addons/\n*.pbo\n'), existing);
  assert.equal(
    mergedLinesOf(existing, '/Addons/\n*.bisign\n'),
    '/Addons/\r\n*.pbo\r\n\r\n# What an Enfusion mod builds, writes while it runs, and signs with.\r\n*.bisign\r\n',
  );
});

/** Nothing is added to the list on the way to the game, so naming the mod here is what loads it. */
test('a mod made in a workspace is named in its mods, after the ones named there already', () => {
  const source = `{
  "launch": {
    // What every target loads.
    "mods": [
      "@ModX",
      "@ModA"
    ],
    "targets": []
  }
}
`;

  assert.equal(
    withWorkspaceMod(source, name('MyMod')),
    `{
  "launch": {
    // What every target loads.
    "mods": [
      "@ModX",
      "@ModA",
      "@MyMod"
    ],
    "targets": []
  }
}
`,
  );
  // A block written along one line comes out a line per member, the way the form writes a row too.
  assert.equal(
    withWorkspaceMod('{\n  "launch": { "mods": ["@ModX"] }\n}\n', name('MyMod')),
    '{\n  "launch": {\n    "mods": [\n      "@ModX",\n      "@MyMod"\n    ]\n  }\n}\n',
  );
});

test('the list a new workspace starts with takes the first mod made in it', () => {
  const written = withWorkspaceMod(contentOf(workspacePlanOf('P:', 'Mods'), 'workspace.enf'), name('MyMod'));

  assert.match(written ?? '', /"mods": \[\s*"@MyMod"\s*\],/);
  assert.match(written ?? '', /What every target loads, in load order/);
});

test('a mod the workspace names already, with its @ or without, leaves the file as it was', () => {
  const source = '{ "launch": { "mods": ["MyMod"] } }';

  assert.equal(withWorkspaceMod(source, name('MyMod')), source);
});

test('a workspace.enf the form would not write into is not written into to name a mod either', () => {
  assert.equal(withWorkspaceMod('{ "launch": { "mods": [ }', name('MyMod')), undefined);
});

/**
 * Where a mod may be made. What a folder already holds decides it, and every refusal is something
 * that would otherwise come out broken without a word: the other mod's pbo carrying the new one,
 * one manifest over several mods, a mod the window never shows.
 */
test('a folder inside a mod takes no mod, wherever in the mod it is', () => {
  const around = surroundings([configured('MyMod', '/w/MyMod')]);

  for (const folder of ['/w/MyMod/MyMod', '/w/MyMod/MyMod/Scripts', '/w/MyMod/Missions']) {
    assert.match(modFolderRefusalOf(folder, around) ?? '', /This folder is inside MyMod\./);
  }
});

test('a folder that is a mod already is told so, and pointed at adding an addon instead', () => {
  const refusal = modFolderRefusalOf('/w/MyMod', surroundings([configured('MyMod', '/w/MyMod')]));

  assert.match(refusal ?? '', /There is a mod here already: this folder is MyMod/);
});

test('a folder holding mods takes no mod over them, and is pointed at Create Workspace', () => {
  const around = surroundings([
    configured('First', '/f:/Code/Repo/First'),
    configured('Second', '/f:/Code/Repo/Second'),
  ]);

  assert.match(
    modFolderRefusalOf('/f:/Code/Repo', around) ?? '',
    /holds First and Second already, and a mod made here would hold them as well.*Create Workspace/,
  );
  // The root of a drive with a repository somewhere below is a folder holding mods like any other.
  assert.match(modFolderRefusalOf('/f:', around) ?? '', /holds First and Second already/);
});

/** The test on the root of a drive: a second mod over a mod nobody configured took its place. */
test('a folder holding a mod nobody configured is pointed at its mod.enf rather than a second mod', () => {
  const around = surroundings([unconfigured('Foreign', '/w/Repo/Foreign')]);

  assert.match(
    modFolderRefusalOf('/w/Repo', around) ?? '',
    /holds Foreign already, a mod found by its config\.cpp.*\+ Create mod\.enf/,
  );
});

/** A repository that is itself `P:\<Name>`: its mod root is above it, and it is the prefix root. */
test('the prefix root of a mod nobody configured is inside that mod', () => {
  const around = surroundings([unconfigured('Foreign', '/w/Foreign')]);

  assert.match(modFolderRefusalOf('/w/Foreign', around) ?? '', /This folder is inside Foreign\./);
});

test('a folder a workspace.enf ignores takes no mod, since this window would never show it', () => {
  const around = surroundings([], ['/w/workspace.enf'], (path) => path.startsWith('/w/Labs/'));

  assert.match(modFolderRefusalOf('/w/Labs', around) ?? '', /ignores, so a mod made here would never be listed/);
});

test('a folder with nothing in it is the mod root itself, and no workspace owns it', () => {
  const around = surroundings([configured('Other', '/w/Other')]);

  assert.equal(modFolderRefusalOf('/w/Fresh', around), undefined);
  assert.deepEqual(modPlacementOf('/w/Fresh', name('Fresh'), around), { root: '/w/Fresh', workspace: undefined });
});

/** A workspace's own folder holds each of its mods in a folder of its own: the new one gets one. */
test('a mod made on a workspace goes into a folder of its own, under the workspace that launches it', () => {
  const around = surroundings(
    [configured('First', '/w/Mods/First'), unconfigured('Foreign', '/w/Mods/Foreign')],
    ['/w/Mods/workspace.enf'],
  );

  assert.equal(modFolderRefusalOf('/w/Mods', around), undefined);
  assert.deepEqual(modPlacementOf('/w/Mods', name('Fresh'), around), {
    root: '/w/Mods/Fresh',
    workspace: '/w/Mods/workspace.enf',
  });
});

test('a mod made on a workspace under a name one of its folders has already is refused', () => {
  const around = surroundings(
    [configured('First', '/w/Mods/First'), unconfigured('Foreign', '/w/Mods/Foreign')],
    ['/w/Mods/workspace.enf'],
  );

  assert.match(modPlacementOf('/w/Mods', name('First'), around).refusal ?? '', /First is a folder of this workspace already/);
  assert.match(modPlacementOf('/w/Mods', name('Foreign'), around).refusal ?? '', /it is Foreign's/);
});

test('a folder of a workspace other than its own is the mod root, and the workspace above owns it', () => {
  const around = surroundings([], ['/w/workspace.enf']);

  assert.deepEqual(modPlacementOf('/w/Group/Fresh', name('Fresh'), around), {
    root: '/w/Group/Fresh',
    workspace: '/w/workspace.enf',
  });
});

/** Paths come from the editor in whatever case the drive letter was typed in. */
test('where a mod may go is worked out the way Windows compares paths', () => {
  const around = surroundings([configured('MyMod', '/f:/Code/MyMod')]);

  assert.match(modFolderRefusalOf('/F:/code/mymod/MyMod', around) ?? '', /inside MyMod/);
});

test('a folder that is a workspace already, or a mod, takes no workspace', () => {
  assert.match(
    workspaceFolderRefusalOf('/w/Mods', surroundings([], ['/w/Mods/workspace.enf'])) ?? '',
    /is a workspace already/,
  );
  assert.match(
    workspaceFolderRefusalOf('/w/MyMod', surroundings([configured('MyMod', '/w/MyMod')])) ?? '',
    /This folder is MyMod\./,
  );
  assert.match(
    workspaceFolderRefusalOf('/w/MyMod/MyMod', surroundings([configured('MyMod', '/w/MyMod')])) ?? '',
    /This folder is inside MyMod\./,
  );
});

/** A workspace owns the launch of every mod under it by existing: one made over theirs takes it. */
test('a workspace is not made over mods that launch by blocks of their own', () => {
  const around = surroundings([
    configured('First', '/w/Repo/First', true),
    configured('Second', '/w/Repo/Second'),
  ]);

  assert.match(
    workspaceFolderRefusalOf('/w/Repo', around) ?? '',
    /First launches by the "launch" block of its own mod\.enf/,
  );
});

test('a workspace is made over mods that launch by nothing, and over a mod nobody configured', () => {
  const around = surroundings([configured('First', '/w/Repo/First'), unconfigured('Foreign', '/w/Repo/Foreign')]);

  assert.equal(workspaceFolderRefusalOf('/w/Repo', around), undefined);
});

test('a workspace is not made over mods another workspace launches', () => {
  const around = surroundings([configured('First', '/w/Group/First')], ['/w/workspace.enf']);

  assert.match(workspaceFolderRefusalOf('/w/Group', around) ?? '', /First is launched by the workspace\.enf above/);
  assert.equal(workspaceFolderRefusalOf('/w/Empty', around), undefined);
});

function surroundings(
  mods: readonly Placed[],
  workspaces: readonly string[] = [],
  ignored: (path: string) => boolean = () => false,
): Surroundings {
  return { mods, workspaces, ignored };
}

/** A mod with its `mod.enf` in its root and its prefix root inside, named after it. */
function configured(modName: string, root: string, launches = false): Placed {
  return { name: modName, root, prefixRoot: `${root}/${modName}`, configured: true, launches };
}

/** A mod found by its `config.cpp`: its root is the prefix root's parent. */
function unconfigured(modName: string, prefixRoot: string): Placed {
  return {
    name: modName,
    root: prefixRoot.slice(0, prefixRoot.lastIndexOf('/')),
    prefixRoot,
    configured: false,
    launches: false,
  };
}

function contentOf(plan: InitPlan, path: string): string {
  return plan.files.find((file) => file.path === path)?.content ?? '';
}

function mainConfig(): string {
  return `class CfgPatches
{
	class MyMod_Scripts
	{
		requiredAddons[] = { "DZ_Scripts" };
	};
};

class CfgMods
{
	class MyMod
	{
		dir = "MyMod";
	};
};
`;
}

function multiAddonMod(): Mod {
  return modOf({
    manifests: ['/w/MyMod/mod.enf'],
    configs: [{ path: '/w/MyMod/MyMod/Scripts/config.cpp', source: mainConfig() }],
  });
}

function singleAddonMod(): Mod {
  return modOf({
    manifests: ['/w/MyMod/mod.enf'],
    configs: [
      {
        path: '/w/MyMod/MyMod/config.cpp',
        source: `class CfgPatches { class MyMod { requiredAddons[] = { "DZ_Scripts" }; }; };
class CfgMods { class MyMod { dir = "MyMod"; }; };`,
      },
    ],
  });
}

/** A mod somebody else wrote: what a `mod.enf` asks for is already written down in it. */
function foreignConfig(): string {
  return `class CfgPatches
{
	class Foreign_Scripts
	{
		units[] = {};
		requiredVersion = 0.1;
		requiredAddons[] = { "DZ_Scripts" };
	};
};

class CfgMods
{
	class Foreign
	{
		type = "mod";
		dir = "Foreign";
		name = "Foreign Mod";
		picture = "";
		overview = "What somebody else made it do.";
		author = "somebody";
		version = "1.4";
	};
};
`;
}

/** The same config wherever it sits, which is what makes the two layouts one case. */
function foreignMod(config: string): Mod {
  return modOf({ manifests: [], configs: [{ path: config, source: foreignConfig() }] });
}

function adoptedManifest(): string {
  return `{
  // The mod's name: what the panel shows, what it goes onto the work drive as (P:\\Foreign)
  // and what it is built into (@Foreign). The folder's own name when left out.
  "name": "Foreign",
  "version": "1.4",
  "description": "What somebody else made it do.",
  "author": "somebody",

  "launch": {
    // Where the built mod goes, counted from this file: Addons\\@Foreign.
    "modsDirectory": "Addons",
    // What every target loads, in load order: nothing is added to this list on the way to the
    // game, so the mod itself is named here, and any mod it needs goes in front of it.
    "mods": ["@Foreign"],
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

function modOf(scan: Parameters<typeof modsFromScan>[0]): Mod {
  const mod = modsFromScan(scan)[0];
  assert.ok(mod);
  return mod;
}

function name(value: string): ModName {
  const checked = modNameOf(value);
  assert.ok(checked !== undefined);
  return checked;
}
