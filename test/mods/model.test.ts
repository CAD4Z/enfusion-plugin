import assert from 'node:assert/strict';
import { test } from 'node:test';
import { modsFromScan, pboNameOf } from '../../src/mods/model';

test('a mod is a folder with mod.enf, and a config.cpp in its prefix root is one addon', () => {
  const mods = modsFromScan({
    manifests: ['/w/ModA/mod.enf'],
    configs: [
      { path: '/w/ModA/ModA/config.cpp', source: config({ patch: 'ModA', dir: 'ModA' }) },
    ],
  });

  assert.deepEqual(mods, [
    {
      name: 'ModA',
      modName: 'ModA',
      root: '/w/ModA',
      manifest: '/w/ModA/mod.enf',
      prefixRoot: '/w/ModA/ModA',
      layout: 'single',
      addons: [
        {
          name: 'ModA',
          root: '/w/ModA/ModA',
          config: '/w/ModA/ModA/config.cpp',
          main: true,
          patches: ['ModA'],
          requires: [],
          unresolved: [],
        },
      ],
      problems: [],
    },
  ]);
});

test('the same tree with no config.cpp in the prefix root makes the subfolders the addons', () => {
  const mods = modsFromScan({
    manifests: ['/w/ModA/mod.enf'],
    configs: [
      {
        path: '/w/ModA/ModA/Scripts/config.cpp',
        source: config({ patch: 'ModA_Scripts', dir: 'ModA' }),
      },
      {
        path: '/w/ModA/ModA/Data/config.cpp',
        source: config({ patch: 'ModA_Data', requires: ['ModA_Scripts'] }),
      },
    ],
  });

  assert.deepEqual(mods, [
    {
      name: 'ModA',
      modName: 'ModA',
      root: '/w/ModA',
      manifest: '/w/ModA/mod.enf',
      prefixRoot: '/w/ModA/ModA',
      layout: 'multi',
      addons: [
        {
          name: 'Scripts',
          root: '/w/ModA/ModA/Scripts',
          config: '/w/ModA/ModA/Scripts/config.cpp',
          main: true,
          patches: ['ModA_Scripts'],
          requires: [],
          unresolved: [],
        },
        {
          name: 'Data',
          root: '/w/ModA/ModA/Data',
          config: '/w/ModA/ModA/Data/config.cpp',
          main: false,
          patches: ['ModA_Data'],
          requires: ['ModA_Scripts'],
          unresolved: [],
        },
      ],
      problems: [],
    },
  ]);
});

test('a mod requiring an addon of its neighbour is listed after it, whatever the scan order', () => {
  const mods = modsFromScan({
    manifests: ['/w/ModB/mod.enf', '/w/ModA/mod.enf'],
    configs: [
      {
        path: '/w/ModB/ModB/config.cpp',
        source: config({ patch: 'ModB', requires: ['ModA'], dir: 'ModB' }),
      },
      {
        path: '/w/ModA/ModA/config.cpp',
        source: config({ patch: 'ModA', dir: 'ModA' }),
      },
    ],
  });

  assert.deepEqual(
    mods.map((mod) => mod.name),
    ['ModA', 'ModB'],
  );
});

test('addons of one mod are ordered by the same graph, and a vanilla name stays unresolved', () => {
  const mods = modsFromScan({
    manifests: ['/w/Big/mod.enf'],
    configs: [
      {
        path: '/w/Big/Big/Data/config.cpp',
        source: config({ patch: 'Big_Data', requires: ['DZ_Data', 'Big_Scripts'] }),
      },
      {
        path: '/w/Big/Big/Scripts/config.cpp',
        source: config({ patch: 'Big_Scripts', requires: ['DZ_Scripts'], dir: 'Big' }),
      },
    ],
  });

  assert.deepEqual(
    mods[0]?.addons.map((addon) => [addon.name, addon.unresolved]),
    [
      ['Scripts', ['DZ_Scripts']],
      ['Data', ['DZ_Data']],
    ],
  );
});

test('two mods requiring each other stay in the list, marked with the cycle they are in', () => {
  const mods = modsFromScan({
    manifests: ['/w/ModA/mod.enf', '/w/ModB/mod.enf'],
    configs: [
      {
        path: '/w/ModA/ModA/config.cpp',
        source: config({ patch: 'ModA', requires: ['ModB'], dir: 'ModA' }),
      },
      {
        path: '/w/ModB/ModB/config.cpp',
        source: config({ patch: 'ModB', requires: ['ModA'], dir: 'ModB' }),
      },
    ],
  });

  assert.deepEqual(
    mods.map((mod) => mod.name).sort(),
    ['ModA', 'ModB'],
  );
  assert.deepEqual(
    mods.map((mod) => mod.problems),
    [
      [{ kind: 'cycle', patches: ['ModA', 'ModB'] }],
      [{ kind: 'cycle', patches: ['ModA', 'ModB'] }],
    ],
  );
});

test('a mod whose config.cpp declares it but which has no mod.enf is listed unconfigured', () => {
  const mods = modsFromScan({
    manifests: [],
    configs: [
      {
        path: '/w/Foreign/Foreign/config.cpp',
        source: config({ patch: 'Foreign', dir: 'Foreign' }),
      },
    ],
  });

  assert.deepEqual(mods, [
    {
      name: 'Foreign',
      modName: 'Foreign',
      root: '/w/Foreign',
      manifest: undefined,
      prefixRoot: '/w/Foreign/Foreign',
      layout: 'single',
      addons: [
        {
          name: 'Foreign',
          root: '/w/Foreign/Foreign',
          config: '/w/Foreign/Foreign/config.cpp',
          main: true,
          patches: ['Foreign'],
          requires: [],
          unresolved: [],
        },
      ],
      problems: [],
    },
  ]);
});

test('a config.cpp that declares no mod is neither an addon of one nor a mod of its own', () => {
  const mods = modsFromScan({
    manifests: ['/w/ModA/mod.enf'],
    configs: [
      {
        path: '/w/ModA/ModA/config.cpp',
        source: config({ patch: 'ModA', dir: 'ModA' }),
      },
      // A mission of the mod: a config.cpp under the mod root, but outside the prefix root.
      {
        path: '/w/ModA/Missions/ModA.sakhal/config.cpp',
        source: config({ patch: 'DZ_Worlds_Sakhal_CE' }),
      },
      // Something a monorepo carries that is no mod at all.
      { path: '/w/client-shop/vendor/config.cpp', source: config({ patch: 'Vendored' }) },
    ],
  });

  assert.deepEqual(
    mods.map((mod) => [mod.name, mod.addons.map((addon) => addon.name)]),
    [['ModA', ['ModA']]],
  );
});

test('a mod root with nothing that packs into a pbo is listed with the problem, not left out', () => {
  const mods = modsFromScan({ manifests: ['/w/Fresh/mod.enf'], configs: [] });

  assert.deepEqual(mods, [
    {
      name: 'Fresh',
      modName: 'Fresh',
      root: '/w/Fresh',
      manifest: '/w/Fresh/mod.enf',
      prefixRoot: undefined,
      layout: undefined,
      addons: [],
      problems: [{ kind: 'no-addons' }],
    },
  ]);
});

test('mods the graph does not relate are listed by name, not in the order the scan found them', () => {
  const mods = modsFromScan({
    manifests: ['/w/Zulu/mod.enf', '/w/Alpha/mod.enf'],
    configs: [
      { path: '/w/Zulu/Zulu/config.cpp', source: config({ patch: 'Zulu', dir: 'Zulu' }) },
      { path: '/w/Alpha/Alpha/config.cpp', source: config({ patch: 'Alpha', dir: 'Alpha' }) },
    ],
  });

  assert.deepEqual(
    mods.map((mod) => mod.name),
    ['Alpha', 'Zulu'],
  );
});

test('the name the manifest declares is the mod, whatever the folder is called', () => {
  const mods = modsFromScan({
    manifests: ['/w/Repo/client/mod.enf'],
    configs: [
      {
        path: '/w/Repo/client/config.cpp',
        source: config({ patch: 'ModC', dir: 'ModC' }),
      },
    ],
    declared: new Map([['/w/Repo/client/mod.enf', 'ModC']]),
  });

  assert.equal(mods[0]?.name, 'ModC');
  // The folder that goes onto the work drive is still the folder: it is what is linked, and it is
  // linked under the name, not renamed to it.
  assert.equal(mods[0]?.prefixRoot, '/w/Repo/client');
});

test('a manifest that declares no name leaves the mod named after its folder', () => {
  const mods = modsFromScan({
    manifests: ['/w/ModA/mod.enf'],
    configs: [
      {
        path: '/w/ModA/ModA/config.cpp',
        source: config({ patch: 'ModA', dir: 'ModA' }),
      },
    ],
    declared: new Map(),
  });

  assert.equal(mods[0]?.name, 'ModA');
});

test('an invalid declared name stays on the mod and is never replaced with its folder', () => {
  for (const declared of ['../Victim', '']) {
    const mod = modsFromScan({
      manifests: ['/w/SafeFolder/mod.enf'],
      configs: [],
      declared: new Map([['/w/SafeFolder/mod.enf', declared]]),
    })[0];

    assert.ok(mod !== undefined);
    assert.equal(mod.name, declared);
    assert.equal(mod.modName, undefined);
    assert.equal(mod.problems[0]?.kind, 'invalid-name');
  }
});

test('a folder fallback is checked by the same rule as a declared name', () => {
  const mod = modsFromScan({ manifests: ['/w/Bad-Mod/mod.enf'], configs: [] })[0];

  assert.ok(mod !== undefined);
  assert.equal(mod.name, 'Bad-Mod');
  assert.equal(mod.modName, undefined);
  assert.equal(mod.problems[0]?.kind, 'invalid-name');
});

/** The two name the same folder, so either of them finding it is enough. */
test('the declared name finds the prefix root where the config does not name it', () => {
  const mods = modsFromScan({
    manifests: ['/w/Mod/mod.enf'],
    configs: [
      {
        path: '/w/Mod/Alpha/Scripts/config.cpp',
        source: config({ patch: 'Alpha_Scripts', dir: 'NotAFolderHere' }),
      },
      {
        path: '/w/Mod/Alpha/Data/config.cpp',
        source: config({ patch: 'Alpha_Data', requires: ['Alpha_Scripts'] }),
      },
    ],
    declared: new Map([['/w/Mod/mod.enf', 'Alpha']]),
  });

  assert.equal(mods[0]?.name, 'Alpha');
  assert.equal(mods[0]?.prefixRoot, '/w/Mod/Alpha');
  assert.deepEqual(
    mods[0]?.addons.map((addon) => addon.name),
    ['Scripts', 'Data'],
  );
});

/** Two mods of one project, told apart by what each of them declares and not by their folders. */
test('mods sit in the order of the names they declare, not of the folders they are in', () => {
  const mods = modsFromScan({
    manifests: ['/w/Nav/server/mod.enf', '/w/Nav/client/mod.enf'],
    configs: [
      { path: '/w/Nav/server/config.cpp', source: config({ patch: 'NavServer', dir: 'NavServer' }) },
      { path: '/w/Nav/client/config.cpp', source: config({ patch: 'NavClient', dir: 'NavClient' }) },
    ],
    declared: new Map([
      ['/w/Nav/server/mod.enf', 'NavServer'],
      ['/w/Nav/client/mod.enf', 'NavClient'],
    ]),
  });

  assert.deepEqual(
    mods.map((mod) => mod.name),
    ['NavClient', 'NavServer'],
  );
});

/**
 * Which pbo an addon ends up in is not a question about its folder alone: the builder is pointed
 * at a path on the work drive, and the prefix root sits there under the mod's name.
 */
test('the addon that is the prefix root packs into a pbo named after the mod', () => {
  const mods = modsFromScan({
    manifests: ['/w/Nav/client/mod.enf'],
    configs: [
      {
        path: '/w/Nav/client/config.cpp',
        source: config({ patch: 'NavClient', dir: 'NavClient' }),
      },
    ],
    declared: new Map([['/w/Nav/client/mod.enf', 'NavClient']]),
  });
  const mod = mods[0];
  const addon = mod?.addons[0];

  assert.ok(mod && addon);
  assert.equal(addon.name, 'client');
  assert.equal(pboNameOf(mod, addon), 'NavClient');
});

test('an addon inside the prefix root keeps its own folder name for its pbo', () => {
  const mods = modsFromScan({
    manifests: ['/w/ModA/mod.enf'],
    configs: [
      {
        path: '/w/ModA/ModA/Scripts/config.cpp',
        source: config({ patch: 'ModA_Scripts', dir: 'ModA' }),
      },
    ],
  });
  const mod = mods[0];
  const addon = mod?.addons[0];

  assert.ok(mod && addon);
  assert.equal(pboNameOf(mod, addon), 'Scripts');
});

/**
 * A `mod.enf` below another mod root is a mod of its own, and what sits under it is its own. The
 * mod above used to reach down and take the first config it found there, which made it whichever
 * of the two the search happened to hand over first.
 */
test('a mod below another mod root keeps its own addons, whatever order the scan found them in', () => {
  const configs = [
    { path: '/w/Outer/Outer/config.cpp', source: config({ patch: 'Outer', dir: 'Outer' }) },
    { path: '/w/Outer/Tools/Inner/Inner/config.cpp', source: config({ patch: 'Inner', dir: 'Inner' }) },
  ];
  const scan = (ordered: typeof configs) =>
    modsFromScan({
      manifests: ['/w/Outer/mod.enf', '/w/Outer/Tools/Inner/mod.enf'],
      configs: ordered,
    }).map((mod) => [mod.name, mod.prefixRoot, mod.addons.map((addon) => addon.name)]);

  const expected = [
    ['Inner', '/w/Outer/Tools/Inner/Inner', ['Inner']],
    ['Outer', '/w/Outer/Outer', ['Outer']],
  ];
  assert.deepEqual(scan(configs), expected);
  assert.deepEqual(scan([...configs].reverse()), expected);
});

/** Two configs declaring a mod under one mod root: the one its manifest names is the mod's own. */
test('a mod root holding two declarations takes the one its manifest names, in either scan order', () => {
  const configs = [
    { path: '/w/Repo/Foreign/config.cpp', source: config({ patch: 'Foreign', dir: 'Foreign' }) },
    { path: '/w/Repo/Fresh/config.cpp', source: config({ patch: 'Fresh', dir: 'Fresh' }) },
  ];
  const scan = (ordered: typeof configs) =>
    modsFromScan({
      manifests: ['/w/Repo/mod.enf'],
      configs: ordered,
      declared: new Map([['/w/Repo/mod.enf', 'Fresh']]),
    }).map((mod) => [mod.name, mod.prefixRoot]);

  assert.deepEqual(scan(configs), [['Fresh', '/w/Repo/Fresh']]);
  assert.deepEqual(scan([...configs].reverse()), [['Fresh', '/w/Repo/Fresh']]);
});

/** A `config.cpp` in the shape a real one is written in, with only the parts the model reads. */
function config({
  patch,
  requires = [],
  dir,
}: {
  patch: string;
  requires?: string[];
  dir?: string;
}): string {
  const required = requires.map((addon) => `"${addon}"`).join(', ');
  const patches = `class CfgPatches { class ${patch} { requiredAddons[] = {${required}}; }; };`;
  const declaration =
    dir === undefined ? '' : `class CfgMods { class ${patch} { dir = "${dir}"; }; };`;

  return `${patches}\n${declaration}`;
}
