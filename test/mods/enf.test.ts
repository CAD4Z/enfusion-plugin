import assert from 'node:assert/strict';
import { test } from 'node:test';
import {
  configurationOf,
  configurationsOf,
  readMod,
  readWorkspace,
  unignored,
  workspaceFor,
} from '../../src/mods/enf';

test('reads what a mod says about itself', () => {
  const read = readMod(`{
  "name": "Acme_Mod",
  "description": "The base every other Acme mod builds on",
  "author": "Acme",
  "version": "1.0.0",
  "exclude": ["**/*.psd", "**/*.blend"]
}`);

  assert.deepEqual(read.problems, []);
  assert.deepEqual(read.value, {
    name: 'Acme_Mod',
    description: 'The base every other Acme mod builds on',
    author: 'Acme',
    version: '1.0.0',
    exclude: ['**/*.psd', '**/*.blend'],
    launch: undefined,
  });
});

test('an empty manifest declares a mod all the same, and so does an empty file', () => {
  for (const source of ['{}', '', '\n\t ']) {
    const read = readMod(source);

    assert.deepEqual(read.problems, []);
    assert.deepEqual(read.value, {
      name: undefined,
      description: undefined,
      author: undefined,
      version: undefined,
      exclude: [],
      launch: undefined,
    });
  }
});

test('comments and a trailing comma are what JSONC is read as', () => {
  const read = readMod(`{
  // The mod as the launcher shows it.
  "name": "Acme_Mod",
  /* Sources the builder has no business packing. */
  "exclude": ["**/*.psd",],
}`);

  assert.deepEqual(read.problems, []);
  assert.equal(read.value.name, 'Acme_Mod');
  assert.deepEqual(read.value.exclude, ['**/*.psd']);
});

test('a syntax error is reported where it is, and the rest of the manifest is still read', () => {
  const read = readMod(`{
  "name": "Acme_Mod"
  "author": "Acme"
}`);

  assert.deepEqual(read.problems, [{ message: 'Comma expected.', line: 3, column: 3 }]);
  assert.equal(read.value.name, 'Acme_Mod');
});

test('an invalid mod name stays readable and is reported on the name field', () => {
  const read = readMod('{\n  "name": "../Victim",\n  "author": "Acme"\n}');

  assert.equal(read.value.name, '../Victim');
  assert.deepEqual(read.problems, [
    {
      message:
        'A mod is named by a class as well as by a folder: letters, digits and underscores, ' +
        'starting with a letter or underscore.',
      line: 2,
      column: 11,
    },
  ]);
});

test('a field of the wrong type is reported where it is written, and left unset', () => {
  const read = readMod(`{
  "name": 4,
  "exclude": "**/*.psd"
}`);

  assert.deepEqual(read.problems, [
    { message: '"name" must be a string.', line: 2, column: 11 },
    { message: '"exclude" must be an array of strings.', line: 3, column: 14 },
  ]);
  assert.equal(read.value.name, undefined);
  assert.deepEqual(read.value.exclude, []);
});

test('an item of the wrong type is reported on its own, and the others survive', () => {
  const read = readMod('{ "exclude": ["**/*.psd", 4, "**/*.blend"] }');

  assert.deepEqual(read.problems, [
    { message: 'Every item of "exclude" must be a string.', line: 1, column: 27 },
  ]);
  assert.deepEqual(read.value.exclude, ['**/*.psd', '**/*.blend']);
});

test('a misspelled field is reported rather than silently ignored', () => {
  const read = readMod('{\n  "descriptoin": "typed by hand"\n}');

  assert.deepEqual(read.problems, [{ message: 'Unknown field "descriptoin".', line: 2, column: 3 }]);
});

test('a manifest that is not an object leaves the mod with an empty configuration', () => {
  const read = readMod('["ModA"]');

  assert.deepEqual(read.problems, [
    { message: 'A manifest must be an object.', line: 1, column: 1 },
  ]);
  assert.equal(read.value.name, undefined);
});

test('reads the launch block a mod carries, filling in what a target leaves out', () => {
  const read = readMod(`{
  "launch": {
    "modsDirectory": "F:/DayZ/Mods",
    "mods": ["@ModX"],
    "serverMods": ["@ModW"],
    "targets": [
      {
        "name": "Sakhal",
        "mod": "ModA",
        "map": "sakhal",
        "run": "both",
        "experimental": true,
        "mods": ["@TargetMod"],
        "serverMods": [],
        "serverConfig": "Profiles/Dev/server.cfg"
      },
      { "name": "Server only", "run": "server" },
      { "name": "Whatever the default is" }
    ]
  }
}`);

  assert.deepEqual(read.problems, []);
  assert.deepEqual(read.value.launch, {
    modsDirectory: 'F:/DayZ/Mods',
    mods: ['@ModX'],
    serverMods: ['@ModW'],
    targets: [
      {
        name: 'Sakhal',
        mod: 'ModA',
        map: 'sakhal',
        run: 'both',
        experimental: true,
        mods: ['@TargetMod'],
        serverMods: [],
        serverConfig: 'Profiles/Dev/server.cfg',
      },
      {
        name: 'Server only',
        mod: undefined,
        map: undefined,
        run: 'server',
        experimental: false,
        mods: undefined,
        serverMods: undefined,
        serverConfig: undefined,
      },
      {
        name: 'Whatever the default is',
        mod: undefined,
        map: undefined,
        run: 'both',
        experimental: false,
        mods: undefined,
        serverMods: undefined,
        serverConfig: undefined,
      },
    ],
  });
});

test('an unsafe loaded-mod folder stays readable and is reported where it was written', () => {
  const read = readMod(
    '{\n  "launch": {\n    "mods": ["../Victim", "A-Mod-With-Dashes"]\n  }\n}',
  );

  assert.deepEqual(read.value.launch?.mods, ['../Victim', 'A-Mod-With-Dashes']);
  assert.equal(read.problems.length, 1);
  assert.equal(read.problems[0]?.line, 3);
  assert.match(read.problems[0]?.message ?? '', /one Windows folder name/);
});

test('a target with no name is dropped, because the Run and Debug list is what names it', () => {
  const read = readMod('{ "launch": { "targets": [{ "map": "sakhal" }, { "name": "Sakhal" }] } }');

  assert.deepEqual(read.problems, [
    {
      message: 'A target must have a "name": it is what the Run and Debug list shows.',
      line: 1,
      column: 27,
    },
  ]);
  assert.deepEqual(
    read.value.launch?.targets.map((target) => target.name),
    ['Sakhal'],
  );
});

test('a run mode nobody supports is reported and falls back to running both', () => {
  const read = readMod('{ "launch": { "targets": [{ "name": "Sakhal", "run": "editor" }] } }');

  assert.deepEqual(read.problems, [
    { message: '"run" must be one of: client, server, both.', line: 1, column: 54 },
  ]);
  assert.equal(read.value.launch?.targets[0]?.run, 'both');
});

test('an Experimental flag of the wrong type is reported and stays off', () => {
  const read = readMod(
    '{ "launch": { "targets": [{ "name": "Exp", "experimental": "yes" }] } }',
  );

  assert.deepEqual(read.problems, [
    { message: '"experimental" must be a boolean.', line: 1, column: 60 },
  ]);
  assert.equal(read.value.launch?.targets[0]?.experimental, false);
});

test('a workspace manifest carries the launch block and nothing about a single mod', () => {
  const read = readWorkspace(`{
  "launch": {
    "modsDirectory": "F:/DayZ/Mods",
    "targets": [{ "name": "Sakhal", "mod": "ModA" }]
  }
}`);

  assert.deepEqual(read.problems, []);
  assert.equal(read.value.launch?.modsDirectory, 'F:/DayZ/Mods');
  assert.deepEqual(
    read.value.launch?.targets.map((target) => target.name),
    ['Sakhal'],
  );
});

test('a mod on its own owns its launch block', () => {
  const mod = readMod(
    '{ "launch": { "modsDirectory": "F:/Mods", "targets": [{ "name": "Sakhal" }] } }',
  ).value;

  assert.deepEqual(configurationOf(mod, undefined), {
    manifest: mod,
    launch: {
      modsDirectory: 'F:/Mods',
      mods: [],
      serverMods: [],
      targets: [
        {
          name: 'Sakhal',
          mod: undefined,
          map: undefined,
          run: 'both',
          experimental: false,
          mods: undefined,
          serverMods: undefined,
          serverConfig: undefined,
        },
      ],
    },
  });
});

test('a workspace file owns launch whole: the block in the mod is ignored, not merged', () => {
  const mod = readMod(`{
  "launch": {
    "modsDirectory": "F:/Mods",
    "mods": ["@ModX"],
    "targets": [{ "name": "Sakhal" }]
  }
}`).value;
  const workspace = readWorkspace('{ "launch": { "targets": [{ "name": "Livonia" }] } }').value;

  const configuration = configurationOf(mod, workspace);

  assert.deepEqual(configuration.launch, {
    modsDirectory: undefined,
    mods: [],
    serverMods: [],
    targets: [
      {
        name: 'Livonia',
        mod: undefined,
        map: undefined,
        run: 'both',
        experimental: false,
        mods: undefined,
        serverMods: undefined,
        serverConfig: undefined,
      },
    ],
  });
});

test('the workspace file owns launch by being there, so one without the block leaves none', () => {
  const mod = readMod('{ "launch": { "targets": [{ "name": "Sakhal" }] } }').value;
  const workspace = readWorkspace('{}').value;

  const configuration = configurationOf(mod, workspace);

  assert.deepEqual(configuration.launch, {
    modsDirectory: undefined,
    mods: [],
    serverMods: [],
    targets: [],
  });
});

test('a mod with no launch block of its own and no workspace file still has a configuration', () => {
  const configuration = configurationOf(readMod('{}').value, undefined);

  assert.deepEqual(configuration.launch.targets, []);
});

test('a mod answers to the nearest workspace.enf above it, and to none where there is none', () => {
  const files = ['/w/workspace.enf', '/w/inner/workspace.enf'];

  assert.equal(workspaceFor('/w/inner/ModA', files), '/w/inner/workspace.enf');
  assert.equal(workspaceFor('/w/ModB', files), '/w/workspace.enf');
  assert.equal(workspaceFor('/elsewhere/ModB', files), undefined);
  assert.equal(workspaceFor('/w/ModB', []), undefined);
});

test('a workspace names the folders it ignores, and one written wrong is reported where it is', () => {
  const read = readWorkspace(`{
  "ignore": ["Labs", "Archive/Old", "", "../Elsewhere", "C:/Mods", "**/Cache"]
}`);

  assert.deepEqual(read.value.ignore, ['Labs', 'Archive/Old', '', '../Elsewhere', 'C:/Mods', '**/Cache']);
  assert.deepEqual(read.problems, [
    {
      message: 'An ignored folder has to be named: an empty one would be the whole workspace.',
      line: 2,
      column: 37,
    },
    {
      message: 'An ignored folder is written relative to this file and stays inside its folder.',
      line: 2,
      column: 41,
    },
    {
      message: 'An ignored folder is written relative to this file and stays inside its folder.',
      line: 2,
      column: 57,
    },
    { message: 'An ignored folder is a folder rather than a mask.', line: 2, column: 68 },
  ]);
});

test('a workspace that ignores nothing says so by leaving the field out', () => {
  assert.deepEqual(readWorkspace('{}').value.ignore, []);
  assert.deepEqual(readWorkspace('{ "ignore": "Labs" }').problems.map((problem) => problem.message), [
    '"ignore" must be an array of strings.',
  ]);
});

test('what a workspace ignores is gone from the scan, whatever it is', () => {
  const scan = [
    '/w/workspace.enf',
    '/w/ModA/mod.enf',
    '/w/ModA/config.cpp',
    '/w/ModA/Workbench/dayz.gproj',
    '/w/Labs/workspace.enf',
    '/w/Labs/ModE/mod.enf',
    '/w/Labs/ModE/config.cpp',
    '/w/Labs/Loose/config.cpp',
    '/w/LabsArchive/Old/config.cpp',
  ];
  const workspaces = [
    { path: '/w/workspace.enf', source: '{ "ignore": ["Labs"] }' },
    { path: '/w/Labs/workspace.enf', source: '{ "launch": { "modsDirectory": "P:/Mods" } }' },
  ];

  // The workspace kept in the ignored folder goes with everything else in it, and a folder whose
  // name merely starts the same way stays
  assert.deepEqual(unignored(scan, workspaces), [
    '/w/workspace.enf',
    '/w/ModA/mod.enf',
    '/w/ModA/config.cpp',
    '/w/ModA/Workbench/dayz.gproj',
    '/w/LabsArchive/Old/config.cpp',
  ]);

  // Opened on its own folder, the ignored workspace has nobody to ignore it
  const alone = scan.filter((path) => path.startsWith('/w/Labs/'));

  assert.deepEqual(unignored(alone, [workspaces[1]]), alone);
});

test('an ignored folder is compared the way Windows compares paths', () => {
  const scan = [
    '/f:/Repo/workspace.enf',
    '/f:/Repo/Labs/ModE/mod.enf',
    '/f:/Repo/Tools/Old/A/mod.enf',
    '/f:/Repo/Tools/New/mod.enf',
  ];
  const workspaces = [
    { path: '/f:/Repo/workspace.enf', source: '{ "ignore": ["labs\\\\", "./Tools/Old/"] }' },
  ];

  assert.deepEqual(unignored(scan, workspaces), [
    '/f:/Repo/workspace.enf',
    '/f:/Repo/Tools/New/mod.enf',
  ]);
});

test('a folder is ignored by the workspace that names it, counted from that workspace', () => {
  const scan = ['/a/workspace.enf', '/a/Labs/mod.enf', '/b/workspace.enf', '/b/Labs/mod.enf'];
  const workspaces = [
    { path: '/a/workspace.enf', source: '{ "ignore": ["Labs"] }' },
    { path: '/b/workspace.enf', source: '{}' },
  ];

  assert.deepEqual(unignored(scan, workspaces), ['/a/workspace.enf', '/b/workspace.enf', '/b/Labs/mod.enf']);
});

test('a folder written wrong ignores nothing rather than a guess at what it meant', () => {
  const scan = ['/w/workspace.enf', '/w/Labs/mod.enf', '/elsewhere/Mod/mod.enf'];
  const wrong = { path: '/w/workspace.enf', source: '{ "ignore": ["../elsewhere", "", "**", "/w/Labs"] }' };

  assert.deepEqual(unignored(scan, [wrong]), scan);
  assert.deepEqual(unignored(scan, []), scan);
});

test('the line an editor is pointed at the schema by is a field like any other', () => {
  const read = readMod('{ "$schema": "https://example.invalid/mod.enf.schema.json", "name": "X" }');

  assert.deepEqual(read.problems, []);
  assert.equal(read.value.name, 'X');
});

test('a workspace of mods is configured in one pass, each mod against the file above it', () => {
  const configurations = configurationsOf(
    [
      { root: '/w/ModA', manifest: '/w/ModA/mod.enf' },
      { root: '/w/ModB', manifest: '/w/ModB/mod.enf' },
      { root: '/elsewhere/Alone', manifest: '/elsewhere/Alone/mod.enf' },
      { root: '/w/Foreign', manifest: undefined },
    ],
    [
      { path: '/w/workspace.enf', source: '{ "launch": { "targets": [{ "name": "Livonia" }] } }' },
      {
        path: '/w/ModA/mod.enf',
        source: '{ "name": "Acme_Mod", "launch": { "targets": [{ "name": "Sakhal" }] } }',
      },
      { path: '/w/ModB/mod.enf', source: '{ "descriptoin": "typo" }' },
      {
        path: '/elsewhere/Alone/mod.enf',
        source: '{ "launch": { "targets": [{ "name": "Chernarus" }] } }',
      },
    ],
  );

  // The mod under a workspace.enf launches the way that file says, whatever its own block holds.
  assert.deepEqual(
    configurations.mods.get('/w/ModA/mod.enf')?.configuration.launch.targets.map((t) => t.name),
    ['Livonia'],
  );
  assert.equal(configurations.mods.get('/w/ModA/mod.enf')?.workspace, '/w/workspace.enf');
  assert.equal(
    configurations.mods.get('/w/ModA/mod.enf')?.configuration.manifest.name,
    'Acme_Mod',
  );

  // The one outside it keeps its own launch block, and has no workspace file to name.
  assert.deepEqual(
    configurations.mods
      .get('/elsewhere/Alone/mod.enf')
      ?.configuration.launch.targets.map((t) => t.name),
    ['Chernarus'],
  );
  assert.equal(configurations.mods.get('/elsewhere/Alone/mod.enf')?.workspace, undefined);

  // A mistake in one manifest is reported against that manifest and nothing else.
  assert.deepEqual(configurations.mods.get('/w/ModB/mod.enf')?.problems, [
    { message: 'Unknown field "descriptoin".', line: 1, column: 3 },
  ]);

  // A mod with no mod.enf has no configuration to speak of, and does not fall out of anything.
  assert.equal(configurations.mods.size, 3);

  assert.deepEqual([...configurations.workspaces], [['/w/workspace.enf', []]]);
});

test('a workspace file that is wrong is reported against itself, not against the mods under it', () => {
  const configurations = configurationsOf([{ root: '/w/ModA', manifest: '/w/ModA/mod.enf' }], [
    { path: '/w/workspace.enf', source: '{ "launch": { "targets": [{ "map": "sakhal" }] } }' },
    { path: '/w/ModA/mod.enf', source: '{}' },
  ]);

  assert.deepEqual(configurations.mods.get('/w/ModA/mod.enf')?.problems, []);
  assert.deepEqual(configurations.workspaces.get('/w/workspace.enf'), [
    {
      message: 'A target must have a "name": it is what the Run and Debug list shows.',
      line: 1,
      column: 27,
    },
  ]);
});
