import assert from 'node:assert/strict';
import { test } from 'node:test';
import { modsFromScan } from './model';

test('a mod is a folder with mod.enf, and a config.cpp in its prefix root is one addon', () => {
  const mods = modsFromScan({
    manifests: ['/w/CADCore/mod.enf'],
    configs: [
      { path: '/w/CADCore/CADCore/config.cpp', source: config({ patch: 'CADCore', dir: 'CADCore' }) },
    ],
  });

  assert.deepEqual(mods, [
    {
      name: 'CADCore',
      root: '/w/CADCore',
      manifest: '/w/CADCore/mod.enf',
      prefixRoot: '/w/CADCore/CADCore',
      layout: 'single',
      addons: [
        {
          name: 'CADCore',
          root: '/w/CADCore/CADCore',
          config: '/w/CADCore/CADCore/config.cpp',
          main: true,
          patches: ['CADCore'],
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
    manifests: ['/w/CADCore/mod.enf'],
    configs: [
      {
        path: '/w/CADCore/CADCore/Scripts/config.cpp',
        source: config({ patch: 'CADCore_Scripts', dir: 'CADCore' }),
      },
      {
        path: '/w/CADCore/CADCore/Data/config.cpp',
        source: config({ patch: 'CADCore_Data', requires: ['CADCore_Scripts'] }),
      },
    ],
  });

  assert.deepEqual(mods, [
    {
      name: 'CADCore',
      root: '/w/CADCore',
      manifest: '/w/CADCore/mod.enf',
      prefixRoot: '/w/CADCore/CADCore',
      layout: 'multi',
      addons: [
        {
          name: 'Scripts',
          root: '/w/CADCore/CADCore/Scripts',
          config: '/w/CADCore/CADCore/Scripts/config.cpp',
          main: true,
          patches: ['CADCore_Scripts'],
          requires: [],
          unresolved: [],
        },
        {
          name: 'Data',
          root: '/w/CADCore/CADCore/Data',
          config: '/w/CADCore/CADCore/Data/config.cpp',
          main: false,
          patches: ['CADCore_Data'],
          requires: ['CADCore_Scripts'],
          unresolved: [],
        },
      ],
      problems: [],
    },
  ]);
});

test('a mod requiring an addon of its neighbour is listed after it, whatever the scan order', () => {
  const mods = modsFromScan({
    manifests: ['/w/CADMap/mod.enf', '/w/CADCore/mod.enf'],
    configs: [
      {
        path: '/w/CADMap/CADMap/config.cpp',
        source: config({ patch: 'CADMap', requires: ['CADCore'], dir: 'CADMap' }),
      },
      {
        path: '/w/CADCore/CADCore/config.cpp',
        source: config({ patch: 'CADCore', dir: 'CADCore' }),
      },
    ],
  });

  assert.deepEqual(
    mods.map((mod) => mod.name),
    ['CADCore', 'CADMap'],
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
    manifests: ['/w/CADCore/mod.enf', '/w/CADMap/mod.enf'],
    configs: [
      {
        path: '/w/CADCore/CADCore/config.cpp',
        source: config({ patch: 'CADCore', requires: ['CADMap'], dir: 'CADCore' }),
      },
      {
        path: '/w/CADMap/CADMap/config.cpp',
        source: config({ patch: 'CADMap', requires: ['CADCore'], dir: 'CADMap' }),
      },
    ],
  });

  assert.deepEqual(
    mods.map((mod) => mod.name).sort(),
    ['CADCore', 'CADMap'],
  );
  assert.deepEqual(
    mods.map((mod) => mod.problems),
    [
      [{ kind: 'cycle', patches: ['CADCore', 'CADMap'] }],
      [{ kind: 'cycle', patches: ['CADCore', 'CADMap'] }],
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
    manifests: ['/w/CADCore/mod.enf'],
    configs: [
      {
        path: '/w/CADCore/CADCore/config.cpp',
        source: config({ patch: 'CADCore', dir: 'CADCore' }),
      },
      // A mission of the mod: a config.cpp under the mod root, but outside the prefix root.
      {
        path: '/w/CADCore/Missions/CADCore.sakhal/config.cpp',
        source: config({ patch: 'DZ_Worlds_Sakhal_CE' }),
      },
      // Something a monorepo carries that is no mod at all.
      { path: '/w/client-shop/vendor/config.cpp', source: config({ patch: 'Vendored' }) },
    ],
  });

  assert.deepEqual(
    mods.map((mod) => [mod.name, mod.addons.map((addon) => addon.name)]),
    [['CADCore', ['CADCore']]],
  );
});

test('a mod root with nothing that packs into a pbo is listed with the problem, not left out', () => {
  const mods = modsFromScan({ manifests: ['/w/Fresh/mod.enf'], configs: [] });

  assert.deepEqual(mods, [
    {
      name: 'Fresh',
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
