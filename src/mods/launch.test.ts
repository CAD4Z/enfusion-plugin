import assert from 'node:assert/strict';
import { test } from 'node:test';
import type { Launch, Target } from './enf';
import {
  type FilePatchingPlan,
  type GameEntry,
  type LaunchInput,
  type LaunchMod,
  type LaunchTarget,
  type TargetSource,
  filePatchingPlanOf,
  launchPlanOf,
  runRootOf,
  targetById,
  targetsOf,
} from './launch';
import type { MachineSettings } from './machine';
import type { LinkFact } from './workDrive';

const GAME = 'F:\\SteamLibrary\\steamapps\\common\\DayZ';
const DIAG = `${GAME}\\DayZDiag_x64.exe`;
const RUN = 'C:\\Users\\dev\\AppData\\Local\\Enfusion\\run\\cad4z';
const CORE = mod('CADCore', 'F:\\Code\\cad4z\\CADCore\\CADCore');
const MAP = mod('CADMap', 'F:\\Code\\cad4z\\CADMap\\CADMap');

/**
 * The whole plan for the plainest case there is: one mod, one target, everything in place.
 * Compared entire, because what a launch is worth checking is the command line it will run and the
 * folder it will run it in — not which functions were called on the way there.
 */
test('a client target comes out as the run folder, its links and one command line', () => {
  const plan = launchPlanOf(input());

  assert.deepEqual(plan, {
    refusals: [],
    warnings: [],
    filePatching: {
      root: RUN,
      junctions: [
        { path: `${RUN}\\Addons`, target: `${GAME}\\Addons` },
        { path: `${RUN}\\sakhal`, target: `${GAME}\\sakhal` },
        { path: `${RUN}\\CADCore`, target: CORE.prefixRoot },
      ],
      remove: [],
      copies: [{ from: `${GAME}\\steam_appid.txt`, to: `${RUN}\\steam_appid.txt` }],
      conflicts: [],
    },
    folders: [RUN, `${RUN}\\profiles\\CADCore\\client`],
    processes: [
      {
        role: 'client',
        what: 'Starting the client for Chernarus',
        program: DIAG,
        arguments: [
          '-filePatching',
          '-scriptDebug=true',
          '-newErrorsAreWarnings=1',
          '-doLogs',
          '-adminlog',
          '-nopause',
          '-nosplash',
          '-window',
          `-profiles=${RUN}\\profiles\\CADCore\\client`,
          '-mod=P:\\Mods\\@CADCore',
          '-mission=dayzOffline.chernarusplus',
        ],
        cwd: RUN,
      },
    ],
  });
});

/**
 * The one thing the run folder exists for. A hardcoded list of engine folders is what the previous
 * implementation had, and what it had wrong: `bliss` has not shipped with the game for years and
 * `sakhal` arrived without anybody adding it.
 */
test('the game root is mirrored by listing it, whatever it happens to hold', () => {
  const plan = patching({
    entries: [
      folder('Addons'),
      folder('bliss'),
      folder('sakhal'),
      folder('BattlEye'),
      folder('!Workshop'),
    ],
  });

  assert.deepEqual(
    plan.junctions.map((junction) => junction.path),
    [
      `${RUN}\\Addons`,
      `${RUN}\\bliss`,
      `${RUN}\\sakhal`,
      `${RUN}\\BattlEye`,
      `${RUN}\\!Workshop`,
      `${RUN}\\CADCore`,
    ],
  );
});

test('a world added by a patch is linked without anything being told about it', () => {
  const plan = patching({ entries: [folder('Addons'), folder('enoch'), folder('namalsk')] });

  assert.ok(plan.junctions.some((junction) => junction.target === `${GAME}\\namalsk`));
});

/**
 * The executables and their libraries are what the rest of the root is, and the game finds those
 * through the path it was started by. Copying them would mean copying the installation.
 */
test('only the files the game reads out of its working directory are carried over', () => {
  const plan = patching({
    entries: [
      folder('Addons'),
      file('steam_appid.txt'),
      file('DayZ_x64.exe'),
      file('DayZDiag_x64.exe'),
      file('steam_api64.dll'),
    ],
  });

  assert.deepEqual(plan.copies, [
    { from: `${GAME}\\steam_appid.txt`, to: `${RUN}\\steam_appid.txt` },
  ]);
  assert.deepEqual(
    plan.junctions.map((junction) => junction.path),
    [`${RUN}\\Addons`, `${RUN}\\CADCore`],
  );
});

test('every mod of the workspace is linked onto the run folder by its prefix root', () => {
  const plan = patching({ mods: [CORE, MAP] });

  assert.deepEqual(
    plan.junctions.filter((junction) => junction.path.endsWith('CADCore') || junction.path.endsWith('CADMap')),
    [
      { path: `${RUN}\\CADCore`, target: CORE.prefixRoot },
      { path: `${RUN}\\CADMap`, target: MAP.prefixRoot },
    ],
  );
});

test('a link already pointing where it should is left alone', () => {
  const plan = patching({
    present: new Map<string, LinkFact>([
      ['Addons', { kind: 'link', target: `${GAME}\\Addons` }],
      ['CADCore', { kind: 'link', target: CORE.prefixRoot }],
    ]),
  });

  assert.deepEqual(plan.junctions, [{ path: `${RUN}\\sakhal`, target: `${GAME}\\sakhal` }]);
  assert.deepEqual(plan.remove, []);
});

/** The case a second workspace, a moved mod or a reinstalled game leaves behind. */
test('a link pointing elsewhere is taken off and made again', () => {
  const plan = patching({
    present: new Map<string, LinkFact>([
      ['CADCore', { kind: 'link', target: 'F:\\Code\\old\\CADCore' }],
    ]),
  });

  assert.deepEqual(plan.remove, [`${RUN}\\CADCore`]);
  assert.ok(plan.junctions.some((junction) => junction.target === CORE.prefixRoot));
});

test('a link nobody asks for any more is taken off', () => {
  const plan = patching({
    present: new Map<string, LinkFact>([
      ['bliss', { kind: 'link', target: `${GAME}\\bliss` }],
      ['CADCompass', { kind: 'link', target: 'F:\\Code\\cad4z\\CADCompass\\CADCompass' }],
    ]),
  });

  assert.deepEqual(plan.remove, [`${RUN}\\bliss`, `${RUN}\\CADCompass`]);
});

/**
 * The game writes into its working directory, and what it wrote is not ours to delete. Only links
 * are ever taken off, which is what makes the folder safe to keep a log or a crash dump in.
 */
test('what the game left in the run folder is neither removed nor reported', () => {
  const plan = patching({
    present: new Map<string, LinkFact>([
      ['crash.mdmp', { kind: 'occupied' }],
      ['profiles', { kind: 'occupied' }],
    ]),
  });

  assert.deepEqual(plan.remove, []);
  assert.deepEqual(plan.conflicts, []);
});

test('something real sitting where a junction has to go is a conflict, not a deletion', () => {
  const plan = patching({ present: new Map<string, LinkFact>([['Addons', { kind: 'occupied' }]]) });

  assert.deepEqual(plan.conflicts, [`${RUN}\\Addons`]);
  assert.deepEqual(plan.remove, []);
  assert.ok(!plan.junctions.some((junction) => junction.path === `${RUN}\\Addons`));
});

/** Windows tells neither the case of a name nor the separator of a path apart, and neither does this. */
test('a link is recognised whatever the case it was written in', () => {
  const plan = patching({
    present: new Map<string, LinkFact>([['cadcore', { kind: 'link', target: CORE.prefixRoot.toUpperCase() }]]),
  });

  assert.ok(!plan.junctions.some((junction) => junction.path.endsWith('CADCore')));
  assert.deepEqual(plan.remove, []);
});

test('a mod named after a folder of the game takes the name', () => {
  const addons = mod('Addons', 'F:\\Code\\Addons\\Addons');
  const plan = patching({ mods: [addons] });

  assert.deepEqual(plan.junctions, [
    { path: `${RUN}\\Addons`, target: addons.prefixRoot },
    { path: `${RUN}\\sakhal`, target: `${GAME}\\sakhal` },
  ]);
});

test('the run folder goes under LOCALAPPDATA, one per workspace, unless a setting says otherwise', () => {
  assert.equal(
    runRootOf('', 'C:\\Users\\dev\\AppData\\Local', 'cad4z'),
    'C:\\Users\\dev\\AppData\\Local\\Enfusion\\run\\cad4z',
  );
  assert.equal(runRootOf('D:\\Run', 'C:\\Users\\dev\\AppData\\Local', 'cad4z'), 'D:\\Run\\cad4z');
});

test('a workspace named what a folder cannot be called still gets a folder', () => {
  assert.equal(runRootOf('D:\\Run', '', 'cad4z (Workspace)'), 'D:\\Run\\cad4z (Workspace)');
  assert.equal(runRootOf('D:\\Run', '', 'a/b:c'), 'D:\\Run\\a-b-c');
  assert.equal(runRootOf('D:\\Run', '', '   '), 'D:\\Run\\workspace');
});

test('a client target with no map comes up at the main menu rather than in a mission', () => {
  const plan = launchPlanOf(input({ target: target({ map: undefined }) }));

  assert.ok(!plan.processes[0]?.arguments.some((argument) => argument.startsWith('-mission=')));
});

/**
 * Load order, which is why the third-party mods come first: a mod is loaded after whatever it is
 * built on, and the workspace's own are already in the order the `requiredAddons` graph put them.
 */
test('the third-party mods are handed to the client first, the workspace’s own after them', () => {
  const plan = launchPlanOf(
    input({
      mods: [CORE, MAP],
      target: target({ launch: launch({ clientMods: ['@CF', 'Community-Online-Tools'] }) }),
    }),
  );

  assert.ok(
    plan.processes[0]?.arguments.includes(
      '-mod=P:\\Mods\\@CF;P:\\Mods\\@Community-Online-Tools;P:\\Mods\\@CADCore;P:\\Mods\\@CADMap',
    ),
    plan.processes[0]?.arguments.join(' '),
  );
});

test('a target naming a mod the workspace has not got starts nothing', () => {
  const plan = launchPlanOf(input({ mods: [MAP], target: target({ mod: 'CADCore' }) }));

  assert.deepEqual(plan.refusals, [
    'Chernarus launches CADCore, which is not a mod of this workspace.',
  ]);
  assert.deepEqual(plan.processes, []);
});

test('a relative mods directory is counted from the file that set it', () => {
  const plan = launchPlanOf(
    input({ target: target({ launch: launch({ modsDirectory: '..\\Build' }) }) }),
  );

  assert.ok(
    plan.processes[0]?.arguments.includes('-mod=F:\\Code\\cad4z\\CADCore\\..\\Build\\@CADCore'),
    plan.processes[0]?.arguments.join(' '),
  );
});

test('a target that puts up a server too starts the client and says the server is not started', () => {
  const plan = launchPlanOf(input({ target: target({ run: 'both' }) }));

  assert.deepEqual(plan.warnings, [
    'Chernarus asks for a server as well, and only the client is started: starting the server is ' +
      'not implemented yet.',
  ]);
  assert.equal(plan.processes.length, 1);
});

test('a target that puts up the server alone starts nothing', () => {
  const plan = launchPlanOf(input({ target: target({ run: 'server' }) }));

  assert.deepEqual(plan.refusals, [
    'Chernarus puts up the server alone, and starting the server is not implemented yet.',
  ]);
  assert.deepEqual(plan.processes, []);
});

test('a launch refuses when the work drive is not mounted, and starts nothing at all', () => {
  const plan = launchPlanOf(
    input({ drive: { letter: 'P:', source: 'F:\\Workdrive', at: '', state: 'unmounted' } }),
  );

  assert.deepEqual(plan.refusals, [
    'P: is not mounted, so the mods have no sources to patch from.',
  ]);
  assert.deepEqual(plan.processes, []);
  assert.deepEqual(plan.filePatching.junctions, []);
});

test('a launch refuses when no DayZ installation is set', () => {
  const plan = launchPlanOf(
    input({ settings: settings({ dayz: '' }), game: { present: false, path: '', executable: '' } }),
  );

  assert.deepEqual(plan.refusals, [
    'No DayZ installation is set: fill in enfusion.dayz.path, which is otherwise read from the ' +
      'registry its installer wrote it to.',
  ]);
});

test('a launch refuses when the executable it would start is not there', () => {
  const plan = launchPlanOf(input({ game: { present: false } }));

  assert.deepEqual(plan.refusals, [
    `${DIAG} is not there. File patching needs the diag build of the game, which comes with ` +
      'DayZ Tools; enfusion.dayz.executable names the one to start.',
  ]);
});

test('a launch refuses when nobody said where the built mods are', () => {
  const plan = launchPlanOf(
    input({ target: target({ launch: launch({ modsDirectory: undefined }) }) }),
  );

  assert.deepEqual(plan.refusals, [
    'No mods directory is set: give mod.enf a "launch" block with a "modsDirectory", which is ' +
      'where the built mods are loaded from.',
  ]);
});

test('a quotation mark in what the manifest puts on a command line is refused', () => {
  const plan = launchPlanOf(
    input({ target: target({ launch: launch({ clientMods: ['@CF" -connect=elsewhere'] }) }) }),
  );

  assert.deepEqual(plan.refusals, [
    'mod.enf has a quotation mark in "@CF" -connect=elsewhere", which no path can hold.',
  ]);
});

/**
 * A `workspace.enf` owns the launch of every mod under it, so its targets belong to the file
 * rather than to the mods: read once, and listed once, however many mods obey them.
 */
test('the targets of a workspace file are listed once, not once per mod', () => {
  const block = launch({ targets: [named('Chernarus'), named('Sakhal')] });
  const targets = targetsOf([
    source({ mod: 'CADCore', owner: '/f:/Code/cad4z/workspace.enf', launch: block }),
    source({ mod: 'CADMap', owner: '/f:/Code/cad4z/workspace.enf', launch: block }),
  ]);

  assert.deepEqual(
    targets.map((target) => [target.id, target.mod]),
    [
      ['Chernarus', 'CADCore'],
      ['Sakhal', 'CADCore'],
    ],
  );
});

test('every mod configured by its own mod.enf brings its own targets', () => {
  const targets = targetsOf([
    source({
      mod: 'CADCore',
      owner: '/f:/Code/cad4z/CADCore/mod.enf',
      launch: launch({ targets: [named('Chernarus')] }),
    }),
    source({
      mod: 'CADMap',
      owner: '/f:/Code/cad4z/CADMap/mod.enf',
      launch: launch({ targets: [named('Sakhal')] }),
    }),
  ]);

  assert.deepEqual(
    targets.map((target) => [target.id, target.mod]),
    [
      ['Chernarus', 'CADCore'],
      ['Sakhal', 'CADMap'],
    ],
  );
});

/** Two mods each calling a target "Client" is ordinary; one of them winning the name silently is not. */
test('targets of the same name are told apart by the mod they belong to', () => {
  const targets = targetsOf([
    source({
      mod: 'CADCore',
      owner: '/f:/Code/cad4z/CADCore/mod.enf',
      launch: launch({ targets: [named('Client')] }),
    }),
    source({
      mod: 'CADMap',
      owner: '/f:/Code/cad4z/CADMap/mod.enf',
      launch: launch({ targets: [named('Client')] }),
    }),
  ]);

  assert.deepEqual(
    targets.map((target) => target.id),
    ['CADCore: Client', 'CADMap: Client'],
  );
  assert.equal(targetById(targets, 'CADMap: Client')?.mod, 'CADMap');
});

test('a target names the mod it launches, and defaults to the one that declared it', () => {
  const targets = targetsOf([
    source({
      mod: 'CADCore',
      owner: '/f:/Code/cad4z/CADCore/mod.enf',
      launch: launch({ targets: [named('Chernarus'), { ...named('Map'), mod: 'CADMap' }] }),
    }),
  ]);

  assert.deepEqual(
    targets.map((target) => target.mod),
    ['CADCore', 'CADMap'],
  );
});

test('a mod with no manifest at all brings no targets', () => {
  assert.deepEqual(targetsOf([source({ owner: '', launch: launch({ targets: [named('X')] }) })]), []);
});

test('a configuration naming a target by its plain name still finds it', () => {
  const targets = targetsOf([
    source({ launch: launch({ targets: [named('Chernarus')] }) }),
  ]);

  assert.equal(targetById(targets, 'Chernarus')?.name, 'Chernarus');
  assert.equal(targetById(targets, 'Sakhal'), undefined);
});

function patching(over: Partial<Parameters<typeof filePatchingPlanOf>[0]> = {}): FilePatchingPlan {
  return filePatchingPlanOf({
    root: RUN,
    game: GAME,
    entries: [folder('Addons'), folder('sakhal'), file('steam_appid.txt')],
    mods: [CORE],
    present: new Map(),
    ...over,
  });
}

function input(
  over: Omit<Partial<LaunchInput>, 'game'> & { game?: Partial<LaunchInput['game']> } = {},
): LaunchInput {
  return {
    target: target(),
    mods: [CORE],
    settings: settings(),
    drive: { letter: 'P:', source: 'F:\\Workdrive', at: 'F:\\Workdrive', state: 'mounted' },
    runRoot: RUN,
    present: new Map(),
    ...over,
    game: {
      path: GAME,
      executable: DIAG,
      present: true,
      entries: [folder('Addons'), folder('sakhal'), file('steam_appid.txt')],
      ...over.game,
    },
  };
}

function target(over: Partial<LaunchTarget> = {}): LaunchTarget {
  return {
    id: 'Chernarus',
    name: 'Chernarus',
    mod: 'CADCore',
    map: 'chernarusplus',
    run: 'client',
    serverConfig: undefined,
    launch: launch(),
    configuredIn: 'F:\\Code\\cad4z\\CADCore',
    configuredBy: 'mod.enf',
    ...over,
  };
}

function launch(over: Partial<Launch> = {}): Launch {
  return {
    modsDirectory: 'P:\\Mods',
    clientMods: [],
    serverMods: [],
    targets: [],
    ...over,
  };
}

function source(over: Partial<TargetSource> = {}): TargetSource {
  return {
    mod: 'CADCore',
    owner: '/f:/Code/cad4z/CADCore/mod.enf',
    configuredBy: 'mod.enf',
    configuredIn: 'F:\\Code\\cad4z\\CADCore',
    launch: launch(),
    ...over,
  };
}

function named(name: string): Target {
  return { name, mod: undefined, map: undefined, run: 'both', serverConfig: undefined };
}

function settings(over: Partial<MachineSettings> = {}): MachineSettings {
  return {
    dayz: GAME,
    executable: '',
    dayzTools: 'F:\\DayZ Tools',
    pboProject: 'C:\\Mikero\\bin\\pboProject.exe',
    privateKey: '',
    workDrive: 'F:\\Workdrive',
    workDriveLetter: 'P:',
    filePatchingRoot: '',
    builder: 'pboProject',
    ...over,
  };
}

function mod(name: string, prefixRoot: string): LaunchMod {
  return { name, prefixRoot };
}

function folder(name: string): GameEntry {
  return { name, directory: true };
}

function file(name: string): GameEntry {
  return { name, directory: false };
}
