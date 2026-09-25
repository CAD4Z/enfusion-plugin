import assert from 'node:assert/strict';
import { test } from 'node:test';
import {
  type MachineSettings,
  builderExecutableOf,
  builderOf,
  environmentOf,
  environmentPaths,
  dayzServerRootOf,
  gameProgramOf,
  isWanting,
  missingProgramOf,
  pboProjectExecutableOf,
  workbenchExecutableOf,
  workDriveToolOf,
} from '../../src/mods/machine';

const DAYZ = 'F:\\SteamLibrary\\steamapps\\common\\DayZ';
const DAYZ_EXPERIMENTAL = 'F:\\SteamLibrary\\steamapps\\common\\DayZ Exp';

/** Steam's own name for the folder it puts DayZ Server in, beside DayZ and never inside it. */
const DAYZ_SERVER = 'F:\\SteamLibrary\\steamapps\\common\\DayZServer';
const DAYZ_EXPERIMENTAL_SERVER = 'F:\\SteamLibrary\\steamapps\\common\\DayZ Server Exp';

const SETTINGS: MachineSettings = {
  dayz: DAYZ,
  dayzExperimental: DAYZ_EXPERIMENTAL,
  executable: '',
  dayzServer: '',
  dayzExperimentalServer: '',
  dayzTools: 'F:\\SteamLibrary\\steamapps\\common\\DayZ Tools',
  pboProject: 'C:\\Mikero\\bin\\pboProject.exe',
  signing: true,
  privateKey: 'F:\\Keys\\CAD4Z.biprivatekey',
  workDrive: 'F:\\DayZ\\Workdrive',
  workDriveLetter: 'P:',
  filePatchingRoot: '',
  profiles: '',
  secondClient: { account: '', sandboxie: '', steam: '' },
  builder: 'pboProject',
};

test('DayZ WorkDrive is found inside the configured tools installation', () => {
  assert.equal(
    workDriveToolOf(SETTINGS),
    'F:\\SteamLibrary\\steamapps\\common\\DayZ Tools\\Bin\\WorkDrive\\WorkDrive.exe',
  );
  assert.equal(workDriveToolOf({ ...SETTINGS, dayzTools: '' }), '');
});

test('DayZ Workbench is found inside the configured tools installation', () => {
  assert.equal(
    workbenchExecutableOf(SETTINGS),
    'F:\\SteamLibrary\\steamapps\\common\\DayZ Tools\\Bin\\Workbench\\workbenchApp.exe',
  );
  assert.equal(workbenchExecutableOf({ ...SETTINGS, dayzTools: '' }), '');
});

test('everything the machine was asked for is there', () => {
  const environment = environmentOf(SETTINGS, [
    SETTINGS.dayz,
    SETTINGS.dayzTools,
    SETTINGS.privateKey,
    SETTINGS.workDrive,
    SETTINGS.pboProject,
  ]);

  assert.deepEqual(environment, [
    {
      kind: 'dayz',
      setting: 'enfusion.dayz.path',
      path: SETTINGS.dayz,
      state: 'ok',
      optional: false,
    },
    {
      kind: 'dayzTools',
      setting: 'enfusion.dayzTools.path',
      path: SETTINGS.dayzTools,
      state: 'ok',
      optional: false,
    },
    {
      kind: 'privateKey',
      setting: 'enfusion.signing.privateKey',
      path: SETTINGS.privateKey,
      state: 'ok',
      optional: false,
    },
    {
      kind: 'workDrive',
      setting: 'enfusion.workDrive.source',
      path: SETTINGS.workDrive,
      state: 'ok',
      optional: false,
    },
    {
      kind: 'builder',
      setting: 'enfusion.pboProject.path',
      path: SETTINGS.pboProject,
      state: 'ok',
      optional: false,
    },
  ]);
});

test('a path that was set but is not there is told apart from one nobody set', () => {
  const environment = environmentOf({ ...SETTINGS, dayzTools: '' }, [SETTINGS.dayz]);

  assert.deepEqual(
    environment.map((entry) => [entry.kind, entry.state]),
    [
      ['dayz', 'ok'],
      ['dayzTools', 'unset'],
      ['privateKey', 'missing'],
      ['workDrive', 'missing'],
      ['builder', 'missing'],
    ],
  );
});

/**
 * The builder is the one entry that is not a setting read straight back: pboProject records its
 * own executable, AddonBuilder is found under DayZ Tools, and the row sends the developer to
 * whichever of the two settings would fill in the one they chose.
 */
test('the builder shown is the one that was chosen, and the setting offered is the one that names it', () => {
  const tools = environmentOf({ ...SETTINGS, builder: 'AddonBuilder' }, []);

  assert.deepEqual(tools.at(-1), {
    kind: 'builder',
    setting: 'enfusion.dayzTools.path',
    path: builderExecutableOf({ ...SETTINGS, builder: 'AddonBuilder' }),
    state: 'missing',
    optional: false,
  });
  assert.equal(
    builderExecutableOf({ ...SETTINGS, builder: 'AddonBuilder' }),
    'F:\\SteamLibrary\\steamapps\\common\\DayZ Tools\\Bin\\AddonBuilder\\AddonBuilder.exe',
  );
});

test('a builder nobody can find is unset rather than missing, which is a different sentence', () => {
  const none = environmentOf({ ...SETTINGS, pboProject: '' }, []);

  assert.equal(none.at(-1)?.state, 'unset');
});

test('signing turned off means the pbo goes unsigned, so the key is the one thing that is optional', () => {
  const environment = environmentOf({ ...SETTINGS, signing: false, privateKey: '' }, []);

  assert.deepEqual(
    environment.filter((entry) => entry.optional).map((entry) => [entry.kind, entry.state]),
    [['privateKey', 'unset']],
  );
});

/**
 * The key is wanted exactly as much as the signature it would make. Signing left on and no key set
 * is a build that will hand back an unsigned mod, so the gap is one, and it is shown as one.
 */
test('signing left on makes a key nobody set a gap like any other', () => {
  const environment = environmentOf({ ...SETTINGS, privateKey: '' }, []);

  assert.deepEqual(
    environment.filter((entry) => entry.optional),
    [],
  );
  assert.ok(environment.filter(isWanting).some((entry) => entry.kind === 'privateKey'));
});

test('what wants attention is a gap, not a choice: an unsigned pbo is nobody in the way', () => {
  const environment = environmentOf({ ...SETTINGS, signing: false, privateKey: '', dayzTools: '' }, [
    SETTINGS.dayz,
  ]);

  assert.deepEqual(
    environment.filter(isWanting).map((entry) => entry.kind),
    ['dayzTools', 'workDrive', 'builder'],
  );
});

/** The check on the disk and the report of it are read off one table, so they cannot disagree. */
test('the paths to ask the disk about are the paths the environment is made of', () => {
  const settings = { ...SETTINGS, dayzTools: '' };

  assert.deepEqual(environmentPaths(settings), [
    settings.dayz,
    settings.privateKey,
    settings.workDrive,
    settings.pboProject,
  ]);
  assert.deepEqual(
    environmentOf(settings, environmentPaths(settings)).map((entry) => entry.state),
    ['ok', 'unset', 'ok', 'ok', 'ok'],
  );
});

test('a path is the same path however it was typed, which on Windows is any way at all', () => {
  const environment = environmentOf(
    { ...SETTINGS, dayz: 'F:/steamlibrary/steamapps/common/dayz/' },
    ['F:\\SteamLibrary\\steamapps\\common\\DayZ'],
  );

  assert.equal(environment[0]?.state, 'ok');
});

/**
 * The mistake this exists for is the folder. The same registry key Mikero's installer writes the
 * executable to holds the folder as well, and the folder is what a person reaches for when asked
 * where a program is. Left as it was typed it does not fail as a program that is not there:
 * `start` hands a folder to the shell, which opens it in Explorer and packs nothing.
 */
test('a builder path that names no executable is taken as the folder holding one', () => {
  const folder = 'C:\\Program Files (x86)\\Mikero\\DePboTools\\bin';
  const exe = folder + '\\pboProject.exe';

  assert.equal(pboProjectExecutableOf(folder), exe);
  assert.equal(pboProjectExecutableOf(folder + '\\'), exe);
  assert.equal(builderExecutableOf({ ...SETTINGS, pboProject: folder }), exe);
});

test('a builder path that names an executable is left exactly as it was typed', () => {
  assert.equal(pboProjectExecutableOf(SETTINGS.pboProject), SETTINGS.pboProject);
  // A copy under another name is still the program, and not a folder to look inside.
  assert.equal(pboProjectExecutableOf('C:\\tools\\pbo.EXE'), 'C:\\tools\\pbo.EXE');
});

test('a builder nobody named stays unnamed rather than becoming a bare file name', () => {
  assert.equal(pboProjectExecutableOf(''), '');
});

test('a builder the settings do not name is the one most machines have', () => {
  assert.equal(builderOf('AddonBuilder'), 'AddonBuilder');
  assert.equal(builderOf('pboProject'), 'pboProject');
  assert.equal(builderOf(''), 'pboProject');
});

/** `-filePatching` is honoured by the diag build alone, which is why that is what Debug starts. */
test('a Debug launch is one diag executable playing both parts', () => {
  assert.deepEqual(gameProgramOf(SETTINGS, 'Debug', 'client'), {
    root: DAYZ,
    name: 'DayZDiag_x64.exe',
    path: `${DAYZ}\\DayZDiag_x64.exe`,
  });
  assert.deepEqual(
    gameProgramOf(SETTINGS, 'Debug', 'server'),
    gameProgramOf(SETTINGS, 'Debug', 'client'),
  );
});

test('a Release launch is the two programs a player and a host run, out of their own folders', () => {
  assert.deepEqual(gameProgramOf(SETTINGS, 'Release', 'client'), {
    root: DAYZ,
    name: 'DayZ_x64.exe',
    path: `${DAYZ}\\DayZ_x64.exe`,
  });
  assert.deepEqual(gameProgramOf(SETTINGS, 'Release', 'server'), {
    root: DAYZ_SERVER,
    name: 'DayZServer_x64.exe',
    path: `${DAYZ_SERVER}\\DayZServer_x64.exe`,
  });
});

test('an Experimental target takes both builds from the separate experimental applications', () => {
  assert.deepEqual(
    gameProgramOf(
      { ...SETTINGS, executable: 'D:\\StableDiag\\DayZDiag_x64.exe' },
      'Debug',
      'client',
      true,
    ),
    {
      root: DAYZ_EXPERIMENTAL,
      name: 'DayZDiag_x64.exe',
      path: `${DAYZ_EXPERIMENTAL}\\DayZDiag_x64.exe`,
    },
  );
  assert.deepEqual(gameProgramOf(SETTINGS, 'Release', 'client', true), {
    root: DAYZ_EXPERIMENTAL,
    name: 'DayZ_x64.exe',
    path: `${DAYZ_EXPERIMENTAL}\\DayZ_x64.exe`,
  });
  assert.deepEqual(gameProgramOf(SETTINGS, 'Release', 'server', true), {
    root: DAYZ_EXPERIMENTAL_SERVER,
    name: 'DayZServer_x64.exe',
    path: `${DAYZ_EXPERIMENTAL_SERVER}\\DayZServer_x64.exe`,
  });
});

/** A machine with no installation set still knows what it would have started, which is the line. */
test('a program with no installation behind it keeps the name it would have been started under', () => {
  const nowhere = gameProgramOf({ ...SETTINGS, dayz: '' }, 'Release', 'server');

  assert.deepEqual(nowhere, { root: '', name: 'DayZServer_x64.exe', path: '' });
});

test('the executable setting names the diag build, and a release launch has nothing to override', () => {
  assert.equal(
    gameProgramOf({ ...SETTINGS, executable: 'DayZDiag_x64_2.exe' }, 'Debug', 'client').path,
    `${DAYZ}\\DayZDiag_x64_2.exe`,
  );
  assert.equal(
    gameProgramOf({ ...SETTINGS, executable: 'D:\\Diag\\DayZDiag_x64.exe' }, 'Debug', 'server').path,
    'D:\\Diag\\DayZDiag_x64.exe',
  );
  assert.equal(
    gameProgramOf({ ...SETTINGS, executable: 'D:\\Diag\\DayZDiag_x64.exe' }, 'Release', 'client')
      .path,
    `${DAYZ}\\DayZ_x64.exe`,
  );
});

test('DayZ Server is the folder beside DayZ until a setting says another one', () => {
  assert.equal(dayzServerRootOf(SETTINGS), DAYZ_SERVER);
  assert.equal(
    dayzServerRootOf({ ...SETTINGS, dayzServer: 'D:\\Servers\\DayZ' }),
    'D:\\Servers\\DayZ',
  );
  assert.equal(dayzServerRootOf({ ...SETTINGS, dayz: '' }), '');
  assert.equal(dayzServerRootOf(SETTINGS, true), DAYZ_EXPERIMENTAL_SERVER);
  assert.equal(
    dayzServerRootOf({ ...SETTINGS, dayzExperimentalServer: 'D:\\Servers\\DayZ Exp' }, true),
    'D:\\Servers\\DayZ Exp',
  );
});

/** Each of the three sends a developer somewhere different, so each says which one it is. */
test('a program that is not there says which setting or install would put it there', () => {
  assert.match(missingProgramOf('Debug', 'client', 'DayZDiag_x64.exe'), /enfusion\.dayz\.executable/);
  assert.match(
    missingProgramOf('Release', 'server', 'DayZServer_x64.exe'),
    /enfusion\.dayzServer\.path/,
  );
  assert.match(missingProgramOf('Release', 'client', 'DayZ_x64.exe'), /enfusion\.dayz\.path/);
  assert.match(
    missingProgramOf('Release', 'server', 'DayZServer_x64.exe', true),
    /enfusion\.dayzExperimentalServer\.path/,
  );
});
