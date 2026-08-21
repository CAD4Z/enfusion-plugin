import assert from 'node:assert/strict';
import { test } from 'node:test';
import { configurationOf, configurationsOf, readMod, readWorkspace, workspaceFor } from './enf';

test('reads what a mod says about itself', () => {
  const read = readMod(`{
  "name": "CAD4Z Core",
  "description": "The base every other CAD mod builds on",
  "author": "hurfy",
  "version": "1.0.0",
  "exclude": ["**/*.psd", "**/*.blend"]
}`);

  assert.deepEqual(read.problems, []);
  assert.deepEqual(read.value, {
    name: 'CAD4Z Core',
    description: 'The base every other CAD mod builds on',
    author: 'hurfy',
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
  "name": "CAD4Z Core",
  /* Sources the builder has no business packing. */
  "exclude": ["**/*.psd",],
}`);

  assert.deepEqual(read.problems, []);
  assert.equal(read.value.name, 'CAD4Z Core');
  assert.deepEqual(read.value.exclude, ['**/*.psd']);
});

test('a syntax error is reported where it is, and the rest of the manifest is still read', () => {
  const read = readMod(`{
  "name": "CAD4Z Core"
  "author": "hurfy"
}`);

  assert.deepEqual(read.problems, [{ message: 'Comma expected.', line: 3, column: 3 }]);
  assert.equal(read.value.name, 'CAD4Z Core');
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
  const read = readMod('["CADCore"]');

  assert.deepEqual(read.problems, [
    { message: 'A manifest must be an object.', line: 1, column: 1 },
  ]);
  assert.equal(read.value.name, undefined);
});

test('reads the launch block a mod carries, filling in what a target leaves out', () => {
  const read = readMod(`{
  "launch": {
    "modsDirectory": "F:/DayZ/Mods",
    "clientMods": ["@CF"],
    "serverMods": ["@ServerTools"],
    "targets": [
      {
        "name": "Sakhal",
        "mod": "CADCore",
        "map": "sakhal",
        "run": "both",
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
    clientMods: ['@CF'],
    serverMods: ['@ServerTools'],
    targets: [
      {
        name: 'Sakhal',
        mod: 'CADCore',
        map: 'sakhal',
        run: 'both',
        serverConfig: 'Profiles/Dev/server.cfg',
      },
      {
        name: 'Server only',
        mod: undefined,
        map: undefined,
        run: 'server',
        serverConfig: undefined,
      },
      {
        name: 'Whatever the default is',
        mod: undefined,
        map: undefined,
        run: 'both',
        serverConfig: undefined,
      },
    ],
  });
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

test('a workspace manifest carries the launch block and nothing about a single mod', () => {
  const read = readWorkspace(`{
  "launch": {
    "modsDirectory": "F:/DayZ/Mods",
    "targets": [{ "name": "Sakhal", "mod": "CADCore" }]
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
      clientMods: [],
      serverMods: [],
      targets: [
        { name: 'Sakhal', mod: undefined, map: undefined, run: 'both', serverConfig: undefined },
      ],
    },
  });
});

test('a workspace file owns launch whole: the block in the mod is ignored, not merged', () => {
  const mod = readMod(`{
  "launch": {
    "modsDirectory": "F:/Mods",
    "clientMods": ["@CF"],
    "targets": [{ "name": "Sakhal" }]
  }
}`).value;
  const workspace = readWorkspace('{ "launch": { "targets": [{ "name": "Namalsk" }] } }').value;

  const configuration = configurationOf(mod, workspace);

  assert.deepEqual(configuration.launch, {
    modsDirectory: undefined,
    clientMods: [],
    serverMods: [],
    targets: [
      { name: 'Namalsk', mod: undefined, map: undefined, run: 'both', serverConfig: undefined },
    ],
  });
});

test('the workspace file owns launch by being there, so one without the block leaves none', () => {
  const mod = readMod('{ "launch": { "targets": [{ "name": "Sakhal" }] } }').value;
  const workspace = readWorkspace('{}').value;

  const configuration = configurationOf(mod, workspace);

  assert.deepEqual(configuration.launch, {
    modsDirectory: undefined,
    clientMods: [],
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

  assert.equal(workspaceFor('/w/inner/CADCore', files), '/w/inner/workspace.enf');
  assert.equal(workspaceFor('/w/CADMap', files), '/w/workspace.enf');
  assert.equal(workspaceFor('/elsewhere/CADMap', files), undefined);
  assert.equal(workspaceFor('/w/CADMap', []), undefined);
});

test('the line an editor is pointed at the schema by is a field like any other', () => {
  const read = readMod('{ "$schema": "https://example.invalid/mod.enf.schema.json", "name": "X" }');

  assert.deepEqual(read.problems, []);
  assert.equal(read.value.name, 'X');
});

test('a workspace of mods is configured in one pass, each mod against the file above it', () => {
  const configurations = configurationsOf(
    [
      { root: '/w/CADCore', manifest: '/w/CADCore/mod.enf' },
      { root: '/w/CADMap', manifest: '/w/CADMap/mod.enf' },
      { root: '/elsewhere/Alone', manifest: '/elsewhere/Alone/mod.enf' },
      { root: '/w/Foreign', manifest: undefined },
    ],
    [
      { path: '/w/workspace.enf', source: '{ "launch": { "targets": [{ "name": "Namalsk" }] } }' },
      {
        path: '/w/CADCore/mod.enf',
        source: '{ "name": "CAD4Z Core", "launch": { "targets": [{ "name": "Sakhal" }] } }',
      },
      { path: '/w/CADMap/mod.enf', source: '{ "descriptoin": "typo" }' },
      {
        path: '/elsewhere/Alone/mod.enf',
        source: '{ "launch": { "targets": [{ "name": "Chernarus" }] } }',
      },
    ],
  );

  // The mod under a workspace.enf launches the way that file says, whatever its own block holds.
  assert.deepEqual(
    configurations.mods.get('/w/CADCore/mod.enf')?.configuration.launch.targets.map((t) => t.name),
    ['Namalsk'],
  );
  assert.equal(configurations.mods.get('/w/CADCore/mod.enf')?.workspace, '/w/workspace.enf');
  assert.equal(
    configurations.mods.get('/w/CADCore/mod.enf')?.configuration.manifest.name,
    'CAD4Z Core',
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
  assert.deepEqual(configurations.mods.get('/w/CADMap/mod.enf')?.problems, [
    { message: 'Unknown field "descriptoin".', line: 1, column: 3 },
  ]);

  // A mod with no mod.enf has no configuration to speak of, and does not fall out of anything.
  assert.equal(configurations.mods.size, 3);

  assert.deepEqual([...configurations.workspaces], [['/w/workspace.enf', []]]);
});

test('a workspace file that is wrong is reported against itself, not against the mods under it', () => {
  const configurations = configurationsOf([{ root: '/w/CADCore', manifest: '/w/CADCore/mod.enf' }], [
    { path: '/w/workspace.enf', source: '{ "launch": { "targets": [{ "map": "sakhal" }] } }' },
    { path: '/w/CADCore/mod.enf', source: '{}' },
  ]);

  assert.deepEqual(configurations.mods.get('/w/CADCore/mod.enf')?.problems, []);
  assert.deepEqual(configurations.workspaces.get('/w/workspace.enf'), [
    {
      message: 'A target must have a "name": it is what the Run and Debug list shows.',
      line: 1,
      column: 27,
    },
  ]);
});
