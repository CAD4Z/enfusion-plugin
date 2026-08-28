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
  launchPathsOf,
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
const CORE = mod('CADCore', 'F:\\Code\\cad4z\\CADCore');
const MAP = mod('CADMap', 'F:\\Code\\cad4z\\CADMap');

/** The arguments every client is started with, which no test is about and every one carries. */
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

/** And the server's. */
const SERVER_ARGUMENTS: readonly string[] = [
  '-server',
  '-filePatching',
  '-scriptDebug=true',
  '-newErrorsAreWarnings=1',
  '-doLogs',
  '-adminlog',
  '-nopause',
  '-nosplash',
  '-world=none',
];

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
      root: `${RUN}\\game`,
      junctions: [
        { path: `${RUN}\\game\\Addons`, target: `${GAME}\\Addons` },
        { path: `${RUN}\\game\\sakhal`, target: `${GAME}\\sakhal` },
        { path: `${RUN}\\game\\CADCore`, target: CORE.prefixRoot },
      ],
      remove: [],
      copies: [{ from: `${GAME}\\steam_appid.txt`, to: `${RUN}\\game\\steam_appid.txt` }],
      conflicts: [],
    },
    folders: [`${RUN}\\game`, `${RUN}\\profiles\\CADCore\\client`],
    copies: [
      { from: `${CORE.root}\\Profiles\\Global`, to: `${RUN}\\profiles\\CADCore\\client` },
      { from: `${CORE.root}\\Profiles\\Dev`, to: `${RUN}\\profiles\\CADCore\\client` },
      { from: `${CORE.root}\\Profiles\\Client`, to: `${RUN}\\profiles\\CADCore\\client` },
    ],
    processes: [
      {
        role: 'client',
        what: 'Starting the client for Chernarus',
        program: DIAG,
        arguments: [
          ...CLIENT_ARGUMENTS,
          `-profiles=${RUN}\\profiles\\CADCore\\client`,
          '-mod=P:\\Mods\\@CADCore',
          '-mission=dayzOffline.chernarusplus',
        ],
        cwd: `${RUN}\\game`,
      },
    ],
  });
});

/**
 * And the whole plan for what the ticket is about: one launch, two processes, the server first and
 * a client that joins it. The profile, the mission and the `server.cfg` all come out of the mod
 * the target names, which is what makes the same target the same launch on another machine.
 */
test('a target that puts up both starts the server and a client that joins it', () => {
  const plan = launchPlanOf(input({ target: target({ run: 'both' }) }));

  assert.deepEqual(plan.refusals, []);
  assert.deepEqual(plan.warnings, []);
  assert.deepEqual(plan.folders, [
    `${RUN}\\game`,
    `${RUN}\\profiles\\CADCore\\server`,
    `${RUN}\\profiles\\CADCore\\client`,
    `${RUN}\\missions\\CADCore.chernarusplus`,
  ]);
  assert.deepEqual(plan.copies, [
    { from: `${CORE.root}\\Profiles\\Global`, to: `${RUN}\\profiles\\CADCore\\server` },
    { from: `${CORE.root}\\Profiles\\Dev`, to: `${RUN}\\profiles\\CADCore\\server` },
    { from: `${CORE.root}\\Profiles\\Server`, to: `${RUN}\\profiles\\CADCore\\server` },
    { from: `${CORE.root}\\Profiles\\Maps\\chernarusplus`, to: `${RUN}\\profiles\\CADCore\\server` },
    { from: `${CORE.root}\\Profiles\\Global`, to: `${RUN}\\profiles\\CADCore\\client` },
    { from: `${CORE.root}\\Profiles\\Dev`, to: `${RUN}\\profiles\\CADCore\\client` },
    { from: `${CORE.root}\\Profiles\\Client`, to: `${RUN}\\profiles\\CADCore\\client` },
    {
      from: `${CORE.root}\\Missions\\CADCore.chernarusplus`,
      to: `${RUN}\\missions\\CADCore.chernarusplus`,
    },
    { from: `${CORE.root}\\Missions\\Global`, to: `${RUN}\\missions\\CADCore.chernarusplus` },
    { from: `${CORE.root}\\Missions\\Dev`, to: `${RUN}\\missions\\CADCore.chernarusplus` },
  ]);
  assert.deepEqual(plan.processes, [
    {
      role: 'server',
      what: 'Starting the server for Chernarus',
      program: DIAG,
      arguments: [
        ...SERVER_ARGUMENTS,
        '-port=2302',
        `-config=${CORE.root}\\server.cfg`,
        `-profiles=${RUN}\\profiles\\CADCore\\server`,
        `-mission=${RUN}\\missions\\CADCore.chernarusplus`,
        '-mod=P:\\Mods\\@CADCore',
      ],
      cwd: `${RUN}\\game`,
    },
    {
      role: 'client',
      what: 'Starting the client for Chernarus',
      program: DIAG,
      arguments: [
        ...CLIENT_ARGUMENTS,
        `-profiles=${RUN}\\profiles\\CADCore\\client`,
        '-mod=P:\\Mods\\@CADCore',
        '-connect=127.0.0.1',
        '-port=2302',
      ],
      cwd: `${RUN}\\game`,
    },
  ]);
});

test('a target that puts up the server alone starts the server and nothing else', () => {
  const plan = launchPlanOf(input({ target: target({ run: 'server' }) }));

  assert.deepEqual(
    plan.processes.map((process_) => process_.role),
    ['server'],
  );
  assert.deepEqual(plan.folders, [
    `${RUN}\\game`,
    `${RUN}\\profiles\\CADCore\\server`,
    `${RUN}\\missions\\CADCore.chernarusplus`,
  ]);
});

/**
 * The profile is the one part of a launch a developer reads rather than merely runs, so where it
 * lands is a setting. Pointed at the folder the Workbench plugins use, both toolchains write one
 * profile instead of two that drift — the layout under it, `<Mod>\<role>`, is already the same.
 */
test('the profiles go where the settings say, and the mirror of the game stays where it is', () => {
  const plan = launchPlanOf(
    input({
      target: target({ run: 'both' }),
      settings: settings({ profiles: 'P:\\Profiles' }),
    }),
  );

  assert.deepEqual(plan.folders, [
    `${RUN}\\game`,
    'P:\\Profiles\\CADCore\\server',
    'P:\\Profiles\\CADCore\\client',
    `${RUN}\\missions\\CADCore.chernarusplus`,
  ]);
  assert.ok(
    plan.processes.every((process_) =>
      process_.arguments.some((argument) => argument === `-profiles=P:\\Profiles\\CADCore\\${process_.role}`),
    ),
  );
  // The file patching root is a mirror of the whole installation, and the work drive is the one
  // place it must never be: both builders read every config on that drive.
  assert.equal(plan.filePatching.root, `${RUN}\\game`);
});

/**
 * The game's root holds a `Missions` of its own, and a launch builds a `missions` of its own. On a
 * filesystem that tells neither name apart those would be the one folder — the junction could not
 * be made, or the mission would be written through it into the DayZ installation. So nothing of
 * ours goes inside the folder the game's root is mirrored into.
 */
test('nothing a launch builds sits inside the folder the game root is mirrored into', () => {
  const plan = launchPlanOf(
    input({
      target: target({ run: 'both' }),
      game: { entries: [folder('Addons'), folder('Missions'), folder('profiles')] },
    }),
  );

  assert.deepEqual(plan.filePatching.conflicts, []);
  assert.ok(
    plan.filePatching.junctions.some((junction) => junction.path === `${RUN}\\game\\Missions`),
    plan.filePatching.junctions.map((junction) => junction.path).join(' '),
  );

  const mirrored = `${RUN}\\game\\`.toLowerCase();
  for (const path of [...plan.folders.slice(1), ...plan.copies.map((copy) => copy.to)]) {
    assert.ok(!path.toLowerCase().startsWith(mirrored), path);
  }
});

/** A client that has a server to join has no business loading a mission of its own. */
test('a client joining the local server is not given an offline mission as well', () => {
  const plan = launchPlanOf(input({ target: target({ run: 'both' }) }));
  const client = plan.processes.find((process_) => process_.role === 'client');

  assert.ok(!client?.arguments.some((argument) => argument.startsWith('-mission=')));
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
test('every file of the game root is carried over but the programs and the logs', () => {
  const plan = patching({
    entries: [
      folder('Addons'),
      file('steam_appid.txt'),
      // The two the engine will not start without, and the reason this is a listing and not a list.
      file('DayZSetting.xml'),
      file('dayz.gproj'),
      file('DayZ_x64.exe'),
      file('DayZDiag_x64.exe'),
      file('steam_api64.dll'),
      file('crash_2025-05-04_21-55-23.log'),
    ],
  });

  assert.deepEqual(plan.copies, [
    { from: `${GAME}\\steam_appid.txt`, to: `${RUN}\\steam_appid.txt` },
    { from: `${GAME}\\DayZSetting.xml`, to: `${RUN}\\DayZSetting.xml` },
    { from: `${GAME}\\dayz.gproj`, to: `${RUN}\\dayz.gproj` },
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
  const addons = mod('Addons', 'F:\\Code\\Addons');
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

/** The server runs the same mods the client does, plus the ones only a server ever loads. */
test('the server mods reach -serverMod= and the workspace’s own stay out of it', () => {
  const plan = launchPlanOf(
    input({
      mods: [CORE, MAP],
      target: target({
        run: 'both',
        launch: launch({ clientMods: ['@CF'], serverMods: ['DayZ-Expansion-Licensed', '@VPPAdminTools'] }),
      }),
    }),
  );
  const server = plan.processes.find((process_) => process_.role === 'server');

  assert.ok(
    server?.arguments.includes('-mod=P:\\Mods\\@CF;P:\\Mods\\@CADCore;P:\\Mods\\@CADMap'),
    server?.arguments.join(' '),
  );
  assert.ok(
    server?.arguments.includes(
      '-serverMod=P:\\Mods\\@DayZ-Expansion-Licensed;P:\\Mods\\@VPPAdminTools',
    ),
    server?.arguments.join(' '),
  );
});

/** An empty `-serverMod=` is not the same thing as no `-serverMod=`: the game takes it badly. */
test('a mod list nobody filled in is left off the command line rather than passed empty', () => {
  const plan = launchPlanOf(input({ target: target({ run: 'both' }) }));
  const said = plan.processes.flatMap((process_) => process_.arguments);

  assert.deepEqual(plan.refusals, []);
  assert.ok(!said.some((argument) => argument.startsWith('-serverMod=')), said.join(' '));
  assert.ok(!said.some((argument) => argument.endsWith('=')), said.join(' '));
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

/**
 * The profile and the mission are the target's mod's own. A launch that took them from whatever
 * else the workspace holds would be a different launch on every machine.
 */
test('the profile and the mission come from the target’s mod, not from its neighbours', () => {
  const plan = launchPlanOf(
    input({ mods: [CORE, MAP], target: target({ run: 'both', mod: 'CADMap' }) }),
  );

  assert.ok(
    plan.copies.every((copy) => copy.from.startsWith(MAP.root)),
    plan.copies.map((copy) => copy.from).join(' '),
  );
  assert.deepEqual(plan.folders, [
    `${RUN}\\game`,
    `${RUN}\\profiles\\CADMap\\server`,
    `${RUN}\\profiles\\CADMap\\client`,
    `${RUN}\\missions\\CADMap.chernarusplus`,
  ]);
});

test('a named serverConfig is taken relative to the target’s mod', () => {
  const plan = launchPlanOf(
    input({
      target: target({ run: 'server', serverConfig: 'Configs\\dev.cfg' }),
      found: [
        `${CORE.root}\\Configs\\dev.cfg`,
        'P:\\Mods\\@CADCore\\Addons\\CADCore.pbo',
        `${CORE.root}\\Missions\\CADCore.chernarusplus`,
      ],
    }),
  );

  assert.deepEqual(plan.refusals, []);
  assert.ok(
    plan.processes[0]?.arguments.includes(`-config=${CORE.root}\\Configs\\dev.cfg`),
    plan.processes[0]?.arguments.join(' '),
  );
});

/**
 * A monorepo configures its dev server once, beside the `workspace.enf` that owns the launch, and
 * a mod that wants its own says so by keeping one.
 */
test('a server.cfg the target’s mod has not got is taken from the file that owns the launch', () => {
  const workspace = target({ run: 'server', configuredIn: 'F:\\Code\\cad4z' });
  const plan = launchPlanOf(
    input({
      target: workspace,
      found: [
        'F:\\Code\\cad4z\\server.cfg',
        'P:\\Mods\\@CADCore\\Addons\\CADCore.pbo',
        `${CORE.root}\\Missions\\CADCore.chernarusplus`,
      ],
    }),
  );

  assert.deepEqual(plan.refusals, []);
  assert.ok(
    plan.processes[0]?.arguments.includes('-config=F:\\Code\\cad4z\\server.cfg'),
    plan.processes[0]?.arguments.join(' '),
  );
});

test('a launch refuses when there is no server.cfg to start the server with', () => {
  const plan = launchPlanOf(
    input({
      target: target({ run: 'both', configuredIn: 'F:\\Code\\cad4z' }),
      found: ['P:\\Mods\\@CADCore\\Addons\\CADCore.pbo'],
    }),
  );

  assert.ok(
    plan.refusals.some((refusal) => refusal.includes('no server.cfg')),
    plan.refusals.join(' '),
  );
  assert.ok(
    plan.refusals.some((refusal) => refusal.includes('F:\\Code\\cad4z\\server.cfg')),
    plan.refusals.join(' '),
  );
  assert.deepEqual(plan.processes, []);
});

test('a server target with no world to load a mission of is refused', () => {
  const plan = launchPlanOf(input({ target: target({ run: 'server', map: undefined }) }));

  assert.ok(
    plan.refusals.some((refusal) => refusal.includes('"map"')),
    plan.refusals.join(' '),
  );
  assert.deepEqual(plan.processes, []);
});

/**
 * The failure this exists to prevent: the game comes up without the mod, whatever depended on it
 * fails in a script error, and it reads as a bug in the mod rather than as a mod never built.
 */
test('a mod that is not built stops the launch and is named', () => {
  const plan = launchPlanOf(input({ mods: [CORE, MAP], found: [] }));

  assert.deepEqual(plan.refusals, [
    'CADCore is not built: nothing is at P:\\Mods\\@CADCore\\Addons\\CADCore.pbo. Build it and ' +
      'launch again.',
    'CADMap is not built: nothing is at P:\\Mods\\@CADMap\\Addons\\CADMap.pbo. Build it and ' +
      'launch again.',
  ]);
  assert.deepEqual(plan.processes, []);
});

test('a mod of several addons is not built until every one of its pbo is there', () => {
  const many = { ...CORE, addons: ['CADCore', 'CADCore_Scripts'] };
  const plan = launchPlanOf(
    input({ mods: [many], found: ['P:\\Mods\\@CADCore\\Addons\\CADCore.pbo'] }),
  );

  assert.deepEqual(plan.refusals, [
    'CADCore is not built: nothing is at P:\\Mods\\@CADCore\\Addons\\CADCore_Scripts.pbo. Build ' +
      'it and launch again.',
  ]);
});

test('a third-party mod that is not in the mods directory stops the launch and is named', () => {
  const plan = launchPlanOf(
    input({
      target: target({ run: 'both', launch: launch({ clientMods: ['CF'], serverMods: ['@VPPAdminTools'] }) }),
      found: [
        'P:\\Mods\\@CADCore\\Addons\\CADCore.pbo',
        `${CORE.root}\\server.cfg`,
        `${CORE.root}\\Missions\\CADCore.chernarusplus`,
      ],
    }),
  );

  assert.deepEqual(plan.refusals, [
    '@CF is not in the mods directory: nothing is at P:\\Mods\\@CF.',
    '@VPPAdminTools is not in the mods directory: nothing is at P:\\Mods\\@VPPAdminTools.',
  ]);
});

/** Only the server ever loads them, so a client-only target is not held up over one. */
test('a server mod that is missing does not stop a target that puts up no server', () => {
  const plan = launchPlanOf(
    input({
      target: target({ launch: launch({ serverMods: ['@VPPAdminTools'] }) }),
      found: ['P:\\Mods\\@CADCore\\Addons\\CADCore.pbo'],
    }),
  );

  assert.deepEqual(plan.refusals, []);
});

/** The layers alone make a mission the engine will load, so this is a sentence rather than a stop. */
test('a world the target’s mod keeps no mission for is a warning, not a refusal', () => {
  const plan = launchPlanOf(
    input({
      target: target({ run: 'server' }),
      found: ['P:\\Mods\\@CADCore\\Addons\\CADCore.pbo', `${CORE.root}\\server.cfg`],
    }),
  );

  assert.deepEqual(plan.refusals, []);
  assert.deepEqual(plan.warnings, [
    `Nothing is at ${CORE.root}\\Missions\\CADCore.chernarusplus, so the server starts with ` +
      "whatever the layers of Missions hold and no mission of CADCore's own.",
  ]);
});

/**
 * What the plan wants a yes or no about, which is what the extension asks the disk before making
 * it: the pbo of everything it would load, and the server's two files where a server is put up.
 */
test('the paths a launch asks the disk about are the built mods and the server’s own files', () => {
  assert.deepEqual(launchPathsOf(target({ run: 'client' }), [CORE, MAP]), [
    'P:\\Mods\\@CADCore\\Addons\\CADCore.pbo',
    'P:\\Mods\\@CADMap\\Addons\\CADMap.pbo',
  ]);

  assert.deepEqual(
    launchPathsOf(
      target({ run: 'both', configuredIn: 'F:\\Code\\cad4z', launch: launch({ clientMods: ['@CF'] }) }),
      [CORE],
    ),
    [
      'P:\\Mods\\@CADCore\\Addons\\CADCore.pbo',
      'P:\\Mods\\@CF',
      `${CORE.root}\\server.cfg`,
      'F:\\Code\\cad4z\\server.cfg',
      `${CORE.root}\\Missions\\CADCore.chernarusplus`,
    ],
  );
});

/** A mod whose manifest sits in its own root asks about one `server.cfg`, not the same one twice. */
test('the same server.cfg looked for twice is asked about once', () => {
  assert.deepEqual(launchPathsOf(target({ run: 'server' }), [CORE]), [
    'P:\\Mods\\@CADCore\\Addons\\CADCore.pbo',
    `${CORE.root}\\server.cfg`,
    `${CORE.root}\\Missions\\CADCore.chernarusplus`,
  ]);
});

test('a target naming a mod the workspace has not got starts nothing', () => {
  const plan = launchPlanOf(input({ mods: [MAP], target: target({ mod: 'CADCore' }) }));

  assert.ok(
    plan.refusals.includes('Chernarus launches CADCore, which is not a mod of this workspace.'),
    plan.refusals.join(' '),
  );
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

  assert.ok(
    plan.refusals.includes(
      'mod.enf has a quotation mark in "@CF" -connect=elsewhere", which no path can hold.',
    ),
    plan.refusals.join(' '),
  );
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

/** Everything in place unless a test says otherwise, `found` included: what is asked for is there. */
function input(
  over: Omit<Partial<LaunchInput>, 'game'> & { game?: Partial<LaunchInput['game']> } = {},
): LaunchInput {
  const chosen = over.target ?? target();
  const mods = over.mods ?? [CORE];

  return {
    target: chosen,
    mods,
    settings: settings(),
    drive: { letter: 'P:', source: 'F:\\Workdrive', at: 'F:\\Workdrive', state: 'mounted' },
    runRoot: RUN,
    present: new Map(),
    found: launchPathsOf(chosen, mods),
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
    profiles: '',
    builder: 'pboProject',
    ...over,
  };
}

/** A mod of one addon named after itself, which is the single-addon layout every test but one is. */
function mod(name: string, root: string): LaunchMod {
  return { name, root, prefixRoot: `${root}\\${name}`, addons: [name] };
}

function folder(name: string): GameEntry {
  return { name, directory: true };
}

function file(name: string): GameEntry {
  return { name, directory: false };
}
