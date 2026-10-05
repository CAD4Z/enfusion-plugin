import assert from 'node:assert/strict';
import { test } from 'node:test';
import { type BuildRequest, isSameRequest, nameOf, queued } from '../../../src/mods/build/buildQueue';

const MOD_A: BuildRequest = { kind: 'addon', mod: 'ModA', addon: 'ModA' };
const SCRIPTS: BuildRequest = { kind: 'addon', mod: 'ModA', addon: 'Scripts' };
const ALL: BuildRequest = { kind: 'all' };

test('a press on an empty line joins it', () => {
  assert.deepEqual(queued([], MOD_A), { waiting: [MOD_A], added: true });
});

test('two different addons both wait, in the order they were pressed', () => {
  const first = queued([], SCRIPTS);
  const second = queued(first.waiting, MOD_A);

  assert.deepEqual(second, { waiting: [SCRIPTS, MOD_A], added: true });
});

test('a press for what is already waiting is folded into it', () => {
  assert.deepEqual(queued([MOD_A], MOD_A), { waiting: [MOD_A], added: false });
});

test('the held key costs nothing: the line stays one deep however many presses land', () => {
  const line = [MOD_A, MOD_A, MOD_A, MOD_A].reduce<readonly BuildRequest[]>(
    (waiting, press) => queued(waiting, press).waiting,
    [],
  );

  assert.deepEqual(line, [MOD_A]);
});

test('building the lot swallows the addons waiting on their own', () => {
  assert.deepEqual(queued([SCRIPTS, MOD_A], ALL), { waiting: [ALL], added: true });
});

test('a second press for the lot adds nothing to the first', () => {
  assert.deepEqual(queued([ALL], ALL), { waiting: [ALL], added: false });
});

test('an addon behind a build of everything is already coming', () => {
  assert.deepEqual(queued([ALL], MOD_A), { waiting: [ALL], added: false });
});

test('the line is never touched in place', () => {
  const waiting = [SCRIPTS];
  queued(waiting, MOD_A);

  assert.deepEqual(waiting, [SCRIPTS]);
});

test('the same press is the same press, and the lot is only ever the same as the lot', () => {
  assert.equal(isSameRequest(MOD_A, { ...MOD_A }), true);
  assert.equal(isSameRequest(MOD_A, SCRIPTS), false);
  assert.equal(isSameRequest(ALL, ALL), true);
  assert.equal(isSameRequest(ALL, MOD_A), false);
  assert.equal(isSameRequest(MOD_A, ALL), false);
});

test('an addon of one mod is not an addon of another that goes by the same folder name', () => {
  const other: BuildRequest = { kind: 'addon', mod: 'ModC', addon: 'Scripts' };

  assert.equal(isSameRequest(SCRIPTS, other), false);
});

test('an addon is named the way the panel named it, and a single-addon mod by the mod alone', () => {
  assert.equal(nameOf(SCRIPTS), 'ModA\\Scripts');
  assert.equal(nameOf(MOD_A), 'ModA');
  assert.equal(nameOf(ALL), 'every addon');
});
