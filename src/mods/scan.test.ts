import assert from 'node:assert/strict';
import { test } from 'node:test';
import { modsFromConfigs } from './scan';

test('names a mod after the folder its config.cpp sits in', () => {
  const mods = modsFromConfigs(['/f:/Code/cad4z/CADCore/CADCore/config.cpp']);

  assert.deepEqual(mods, [
    {
      name: 'CADCore',
      root: '/f:/Code/cad4z/CADCore/CADCore',
      config: '/f:/Code/cad4z/CADCore/CADCore/config.cpp',
    },
  ]);
});

test('sorts by name and collapses repeats of the same folder', () => {
  const mods = modsFromConfigs([
    '/p/CADMap/config.cpp',
    '/p/CADCore/config.cpp',
    '/p/CADMap/config.cpp',
  ]);

  assert.deepEqual(
    mods.map((mod) => mod.name),
    ['CADCore', 'CADMap'],
  );
});

test('ignores paths that leave no folder to name a mod after', () => {
  const mods = modsFromConfigs(['config.cpp', '/config.cpp', '/p/CADCore/mod.cpp']);

  assert.deepEqual(mods, []);
});
