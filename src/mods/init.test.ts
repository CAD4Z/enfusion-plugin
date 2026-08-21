import assert from 'node:assert/strict';
import { test } from 'node:test';
import { parseConfig } from './config';
import {
  type AddonPlan,
  type Adoption,
  type InitPlan,
  addonPlanOf,
  adoptionOf,
  initPlanOf,
  modFieldsOf,
  modNameProblemOf,
  requiringAddon,
} from './init';
import { type Mod, modsFromScan } from './model';

/**
 * The whole of a new single-addon mod, compared entire. What matters about an initialisation is
 * what is on the disk afterwards — every folder, every file and every line in it — rather than
 * which of them was written first, so this is the shape the test takes.
 */
test('a single-addon mod comes out whole, with every name in it worked out from the one given', () => {
  assert.deepEqual(initPlanOf('MyMod', 'single'), {
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
    ],
    files: [
      {
        path: 'mod.enf',
        content: `{
  // What the panel and the launcher call this mod; the prefix root's name when left out.
  "name": "MyMod",
  "version": "0.1.0",
  // "description": "What the mod does, in a sentence.",
  // "author": "Who made it.",

  "launch": {
    // Where the built mod goes, counted from this file: Addons\\@MyMod.
    "modsDirectory": "Addons",
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
        path: 'MyMod/stringtable.csv',
        content:
          'Language\toriginal\tenglish\tczech\tgerman\trussian\tpolish\thungarian\titalian\t' +
          'spanish\tfrench\tchinese\tjapanese\tportuguese\tchinesesimp\n',
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
    ],
  } satisfies InitPlan);
});

/**
 * The multi-addon layout differs in one thing only: which folder the `config.cpp` and the
 * stringtable sit in. The paths `CfgMods` carries are the same either way, which is what lets a
 * mod be split up later by moving files rather than by editing every path in the config.
 */
test('a multi-addon mod puts the same files in Scripts, and points CfgMods at the same paths', () => {
  const plan = initPlanOf('MyMod', 'multi');
  const single = initPlanOf('MyMod', 'single');

  assert.deepEqual(
    plan.files.map((file) => file.path),
    [
      'mod.enf',
      '.gitignore',
      'MyMod/Scripts/config.cpp',
      'MyMod/mod.cpp',
      'MyMod/Scripts/stringtable.csv',
      'MyMod/Scripts/Inputs.xml',
      'MyMod/Scripts/1_Core/MyMod.c',
      'MyMod/Scripts/3_Game/MyMod.c',
      'MyMod/Scripts/4_World/MyMod.c',
      'MyMod/Scripts/5_Mission/MyMod.c',
      'Missions/Global/.gitkeep',
      'Profiles/Global/.gitkeep',
      'Profiles/Dev/.gitkeep',
    ],
  );
  assert.deepEqual(plan.folders, single.folders);

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

/** The name is one the developer typed, and it ends up as a class name in three files. */
test('a name that no class could go by is refused, and every other one is taken', () => {
  assert.equal(modNameProblemOf('MyMod'), undefined);
  assert.equal(modNameProblemOf('My_Mod_2'), undefined);

  assert.match(modNameProblemOf('') ?? '', /needs a name/);
  assert.match(modNameProblemOf('   ') ?? '', /needs a name/);
  assert.match(modNameProblemOf('My Mod') ?? '', /letters, digits and underscores/);
  assert.match(modNameProblemOf('2Mods') ?? '', /starting with a letter/);
  assert.match(modNameProblemOf('My-Mod') ?? '', /letters, digits and underscores/);
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
    name: 'Foreign Mod',
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
      name: 'Foreign Mod',
      description: 'What somebody else made it do.',
      author: 'somebody',
      version: '1.4',
    },
    folders: [],
    files: [{ path: 'mod.enf', content: adoptedManifest() }],
    refusal: undefined,
  } satisfies Adoption);
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
  assert.equal(contentOf(adoption, 'mod.enf'), contentOf(initPlanOf('Foreign', 'single'), 'mod.enf'));
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
  // What the panel and the launcher call this mod; the prefix root's name when left out.
  "name": "Foreign Mod",
  "version": "1.4",
  "description": "What somebody else made it do.",
  "author": "somebody",

  "launch": {
    // Where the built mod goes, counted from this file: Addons\\@Foreign.
    "modsDirectory": "Addons",
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
