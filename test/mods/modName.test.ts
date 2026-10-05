import assert from 'node:assert/strict';
import { test } from 'node:test';
import {
  isModNameOf,
  loadedModNameOf,
  loadedModNameProblemOf,
  modNameOf,
  modNameProblemOf,
  modPathOf,
} from '../../src/mods/modName';

test('a mod name is the class-safe folder segment every owned-mod operation shares', () => {
  for (const name of ['MyMod', 'My_Mod_2', '_Private']) {
    assert.equal(modNameOf(name), name);
    assert.equal(modNameProblemOf(name), undefined);
  }

  for (const name of [
    '',
    '   ',
    '2Mods',
    'My Mod',
    'My-Mod',
    '..',
    'A/B',
    'A\\B',
    'CON',
    'con',
    'COM1',
    'lpt9',
  ]) {
    assert.equal(modNameOf(name), undefined, name);
    assert.notEqual(modNameProblemOf(name), undefined, name);
  }
});

test('a checked mod name is the only value source and built paths accept', () => {
  const name = modNameOf('ModA');
  assert.ok(name !== undefined);

  assert.equal(modPathOf('P:', name, 'source'), 'P:\\ModA');
  assert.equal(modPathOf('P:\\Mods', name, 'built'), 'P:\\Mods\\@ModA');
});

test('a prepared name cannot be paired with a different raw identity', () => {
  const name = modNameOf('ModA');
  assert.ok(name !== undefined);

  assert.equal(isModNameOf('ModA', name), true);
  assert.equal(isModNameOf('ModB', name), false);
  assert.equal(isModNameOf('ModA', undefined), false);
});

test('a loaded-mod reference is a safe folder name, not an Enforce class name', () => {
  for (const [written, canonical] of [
    ['ModX', 'ModX'],
    ['@ModX', 'ModX'],
    ['A-Mod-With-Dashes', 'A-Mod-With-Dashes'],
    ['A Mod With Spaces', 'A Mod With Spaces'],
  ] as const) {
    const name = loadedModNameOf(written);
    assert.equal(name, canonical);
    assert.equal(loadedModNameProblemOf(written), undefined);
    assert.ok(name !== undefined);
    assert.equal(modPathOf('P:\\Mods', name, 'built'), `P:\\Mods\\@${canonical}`);
  }
});

test('a loaded-mod reference cannot escape or split the folder it is loaded from', () => {
  for (const name of [
    '',
    '@',
    '.',
    '..',
    '../Victim',
    'A/B',
    'A\\B',
    'C:Victim',
    'Victim;@Other',
    'Victim"',
    'Victim.',
    'Victim ',
    'Victim\nOther',
    'CON',
    'con.txt',
    'LPT9',
  ]) {
    assert.equal(loadedModNameOf(name), undefined, JSON.stringify(name));
    assert.notEqual(loadedModNameProblemOf(name), undefined, JSON.stringify(name));
  }
});
