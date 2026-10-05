import assert from 'node:assert/strict';
import { test } from 'node:test';
import type { ModDefs } from '../../src/mods/config';
import {
  projectFileOf,
  projectOf,
  withProjectMod,
  workbenchArgumentsOf,
} from '../../src/mods/workbench';

const ROOT = '/f:/Code/cad4z/CADCore';

test('the conventional dayz.gproj of the target mod wins over helper projects', () => {
  assert.equal(
    projectOf(ROOT, [
      `${ROOT}/Tools/Preview.gproj`,
      '/f:/Code/cad4z/Another/Workbench/dayz.gproj',
      `${ROOT}/CADCore/Workbench/dayz.gproj`,
    ]),
    `${ROOT}/CADCore/Workbench/dayz.gproj`,
  );
});

test('without dayz.gproj the shallowest stable project is used', () => {
  assert.equal(
    projectOf(ROOT, [
      `${ROOT}/Deep/Workbench/Zeta.gproj`,
      `${ROOT}/Workbench/Beta.gproj`,
      `${ROOT}/Workbench/Alpha.gproj`,
    ]),
    `${ROOT}/Workbench/Alpha.gproj`,
  );
});

test('a project from another mod is never borrowed', () => {
  assert.equal(projectOf(ROOT, ['/f:/Code/cad4z/CADMap/Workbench/dayz.gproj']), undefined);
});

/** One project for every mod of a workspace, which is what lets one Workbench compile them all. */
test('a mod with no project of its own opens its workspace’s, and never another mod’s', () => {
  const first = '/w/Mods/First/Workbench/dayz.gproj';

  assert.equal(
    projectOf('/w/Mods/Fresh', [first, '/w/Mods/Workbench/dayz.gproj'], '/w/Mods'),
    '/w/Mods/Workbench/dayz.gproj',
  );
  assert.equal(projectOf('/w/Mods/Fresh', [first], '/w/Mods'), undefined);
  assert.equal(projectOf('/w/Mods/First', [first, '/w/Mods/Workbench/dayz.gproj'], '/w/Mods'), first);
});

/** A copy of a project to try something on is not the workspace's project, wherever it sits. */
test('a workspace’s project is the one in its Workbench folder, and no other project in it', () => {
  assert.equal(projectOf('/w/Mods/Fresh', ['/w/Mods/.scratch/probe/dayz.gproj'], '/w/Mods'), undefined);
  assert.equal(projectOf('/w/Mods/Fresh', ['/w/Mods/Workbench/Tools/dayz.gproj'], '/w/Mods'), undefined);
});

test('the repository is one quoted argument, spaces and all', () => {
  assert.equal(
    workbenchArgumentsOf('F:\\Code\\my mods\\CADCore'),
    '-doLogs "-repository=F:\\Code\\my mods\\CADCore"',
  );
});

/** `\"` is a quotation mark kept rather than a quote closed, so the backslash before is doubled. */
test('a repository ending in a backslash still closes its quote', () => {
  assert.equal(workbenchArgumentsOf('F:\\'), '-doLogs "-repository=F:\\\\"');
});

/**
 * The whole of a project, compared entire: DayZ Tools' own `dayz.gproj`, with the work drive as its
 * file system and the mod's folders after the vanilla ones in the modules its config attaches.
 */
test('a project is DayZ’s own, on the work drive, with the mod’s scripts after the vanilla ones', () => {
  assert.equal(
    projectFileOf('P:', 'MyMod', [scriptsOf('MyMod')]),
    `GameProjectClass {
	ID "DayZ"
	TITLE "MyMod"
	Configurations {
		GameProjectConfigClass PC {
			platformHardware PC
			skeletonDefinitions "DZ/Anims/cfg/skeletons.anim.xml"
			FileSystem {
				FileSystemPathClass {
					Name "Workdrive"
					Directory "P:/"
				}
			}
			imageSets {
				"gui/imagesets/BleedingDrops.imageset"
				"gui/imagesets/ccgui_enforce.imageset"
				"gui/imagesets/rover_imageset.imageset"
				"gui/imagesets/dayz_gui.imageset"
				"gui/imagesets/dayz_crosshairs.imageset"
				"gui/imagesets/dayz_inventory.imageset"
				"gui/imagesets/xbox_buttons.imageset"
				"gui/imagesets/playstation_buttons.imageset"
				"gui/imagesets/console_toolbar.imageset"
				"gui/imagesets/Map2D_UI.imageset"
				"Graphics/Textures/postprocess/VignetteFrames.imageset"
				"gui/imagesets/dayz_additional_gui.imageset"
			}
			widgetStyles {
				"gui/looknfeel/dayzwidgets.styles"
			}
			PhysicsSettings PhysicsSettingsClass "{50C8B06FB4D5FA52}" {
				GridBP 0
				Optimize 16
			}
			ScriptModules {
				ScriptModulePathClass {
					Name "core"
					Paths {
						"scripts/1_Core"
						"MyMod/Scripts/1_Core"
					}
					EntryPoint ""
				}
				ScriptModulePathClass {
					Name "gameLib"
					Paths {
						"scripts/2_GameLib"
					}
					EntryPoint ""
				}
				ScriptModulePathClass {
					Name "game"
					Paths {
						"scripts/3_Game"
						"MyMod/Scripts/3_Game"
					}
					EntryPoint "CreateGame"
				}
				ScriptModulePathClass {
					Name "world"
					Paths {
						"scripts/4_World"
						"MyMod/Scripts/4_World"
					}
					EntryPoint ""
				}
				ScriptModulePathClass {
					Name "mission"
					Paths {
						"scripts/5_Mission"
						"MyMod/Scripts/5_Mission"
					}
					EntryPoint "CreateMission"
				}
				ScriptModulePathClass {
					Name "workbench"
					Paths {
						"scripts/editor/Workbench"
						"scripts/editor/plugins"
					}
					EntryPoint ""
				}
			}
		}
		GameProjectConfigClass XBOX_ONE {
			platformHardware XBOX_ONE
		}
		GameProjectConfigClass PS4 {
			platformHardware PS4
		}
		GameProjectConfigClass LINUX {
			platformHardware LINUX
		}
	}
}
`,
  );
});

test('the mods a project lists come in the order given, each folder once, with their image sets', () => {
  const ui: ModDefs = { ...scriptsOf('Second'), imageSets: ['Second/gui/second.imageset'] };
  const project = projectFileOf('Q:', 'Mods', [scriptsOf('First'), ui, scriptsOf('first')]);

  assert.match(project, /Directory "Q:\/"/);
  assert.match(project, /"scripts\/3_Game"\n\t+"First\/Scripts\/3_Game"\n\t+"Second\/Scripts\/3_Game"\n\t+\}/);
  assert.match(project, /"gui\/imagesets\/dayz_additional_gui\.imageset"\n\t+"Second\/gui\/second\.imageset"\n/);
});

/**
 * A mod made in a workspace is written into the project the workspace already has, and comes out
 * the same as a project written for both from the start: which is the whole of what the edit is.
 */
test('a mod written into a project comes out as the project written with it from the start', () => {
  const empty = projectFileOf('P:', 'Mods', []);
  const first = withProjectMod(empty, scriptsOf('First'));

  assert.equal(first, projectFileOf('P:', 'Mods', [scriptsOf('First')]));
  assert.equal(
    withProjectMod(first ?? '', scriptsOf('Second')),
    projectFileOf('P:', 'Mods', [scriptsOf('First'), scriptsOf('Second')]),
  );
  // Written in already, the same mod changes nothing.
  assert.equal(withProjectMod(first ?? '', scriptsOf('First')), first);
});

/** A project written by hand: its own layout, its own plugins, a comment with a brace in it. */
test('a mod written into a project somebody else wrote goes where its lists are, and nowhere else', () => {
  const handWritten = [
    'GameProjectClass {',
    '\tID "CAD4Z"',
    '\tConfigurations {',
    '\t\tGameProjectConfigClass PC {',
    '\t\t\tplatformHardware PC',
    '\t\t\t// The work drive { mounted by DayZ Tools }',
    '\t\t\tScriptModules {',
    '\t\t\t\tScriptModulePathClass {',
    '\t\t\t\t\tName "core"',
    '\t\t\t\t\tPaths { "scripts/1_Core" }',
    '\t\t\t\t\tEntryPoint ""',
    '\t\t\t\t}',
    '\t\t\t\tScriptModulePathClass {',
    '\t\t\t\t\tName "game"',
    '\t\t\t\t\tPaths {',
    '\t\t\t\t\t\t"scripts/3_Game"',
    '\t\t\t\t\t\t"CADCore/Scripts/3_Game"',
    '\t\t\t\t\t}',
    '\t\t\t\t\tEntryPoint "CreateGame"',
    '\t\t\t\t}',
    '\t\t\t\tScriptModulePathClass {',
    '\t\t\t\t\tName "workbench"',
    '\t\t\t\t\tPaths {',
    '\t\t\t\t\t\t"CADCore/Workbench/ToolAddons"',
    '\t\t\t\t\t}',
    '\t\t\t\t\tEntryPoint "CreateWorkbench"',
    '\t\t\t\t}',
    '\t\t\t}',
    '\t\t}',
    '\t}',
    '}',
    '',
  ].join('\r\n');
  const mod: ModDefs = {
    scripts: { core: ['Fresh/Scripts/1_Core'], gameLib: [], game: ['Fresh/Scripts/3_Game'], world: [], mission: [] },
    imageSets: ['Fresh/gui/fresh.imageset'],
    widgetStyles: [],
  };

  assert.equal(
    withProjectMod(handWritten, mod),
    handWritten
      .replace('Paths { "scripts/1_Core" }', 'Paths { "scripts/1_Core" "Fresh/Scripts/1_Core" }')
      .replace(
        '\t\t\t\t\t\t"CADCore/Scripts/3_Game"\r\n',
        '\t\t\t\t\t\t"CADCore/Scripts/3_Game"\r\n\t\t\t\t\t\t"Fresh/Scripts/3_Game"\r\n',
      ),
  );
});

/** A brace put on a line of its own leaves the block named by the line above it. */
test('a list whose brace stands on a line of its own is written into all the same', () => {
  const ownLine = (project: string): string => project.replace(/Paths \{/g, 'Paths\n\t\t\t\t\t{');

  assert.equal(
    withProjectMod(ownLine(projectFileOf('P:', 'Mods', [])), scriptsOf('MyMod')),
    ownLine(projectFileOf('P:', 'Mods', [scriptsOf('MyMod')])),
  );
});

test('a project with no list for a module the mod has scripts in is not written into', () => {
  const world = /\t+ScriptModulePathClass \{\n\t+Name "world"[\s\S]*?\n\t+\}\n\t+EntryPoint ""\n\t+\}\n/;
  const noWorld = projectFileOf('P:', 'Mods', []).replace(world, '');

  assert.doesNotMatch(noWorld, /Name "world"/);
  assert.equal(withProjectMod(noWorld, scriptsOf('MyMod')), undefined);
  assert.equal(withProjectMod('nothing like a project', scriptsOf('MyMod')), undefined);
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
