import assert from 'node:assert/strict';
import { test } from 'node:test';
import { type BuildRequest, isSameRequest, nameOf, queued } from './buildQueue';

const CORE: BuildRequest = { kind: 'addon', mod: 'CADCore', addon: 'CADCore' };
const SCRIPTS: BuildRequest = { kind: 'addon', mod: 'CADCore', addon: 'Scripts' };
const ALL: BuildRequest = { kind: 'all' };

test('a press on an empty line joins it', () => {
  assert.deepEqual(queued([], CORE), { waiting: [CORE], added: true });
});

test('two different addons both wait, in the order they were pressed', () => {
  const first = queued([], SCRIPTS);
  const second = queued(first.waiting, CORE);

  assert.deepEqual(second, { waiting: [SCRIPTS, CORE], added: true });
});

test('a press for what is already waiting is folded into it', () => {
  assert.deepEqual(queued([CORE], CORE), { waiting: [CORE], added: false });
});

test('the held key costs nothing: the line stays one deep however many presses land', () => {
  const line = [CORE, CORE, CORE, CORE].reduce<readonly BuildRequest[]>(
    (waiting, press) => queued(waiting, press).waiting,
    [],
  );

  assert.deepEqual(line, [CORE]);
});

test('building the lot swallows the addons waiting on their own', () => {
  assert.deepEqual(queued([SCRIPTS, CORE], ALL), { waiting: [ALL], added: true });
});

test('a second press for the lot adds nothing to the first', () => {
  assert.deepEqual(queued([ALL], ALL), { waiting: [ALL], added: false });
});

test('an addon behind a build of everything is already coming', () => {
  assert.deepEqual(queued([ALL], CORE), { waiting: [ALL], added: false });
});

test('the line is never touched in place', () => {
  const waiting = [SCRIPTS];
  queued(waiting, CORE);

  assert.deepEqual(waiting, [SCRIPTS]);
});

test('the same press is the same press, and the lot is only ever the same as the lot', () => {
  assert.equal(isSameRequest(CORE, { ...CORE }), true);
  assert.equal(isSameRequest(CORE, SCRIPTS), false);
  assert.equal(isSameRequest(ALL, ALL), true);
  assert.equal(isSameRequest(ALL, CORE), false);
  assert.equal(isSameRequest(CORE, ALL), false);
});

test('an addon of one mod is not an addon of another that goes by the same folder name', () => {
  const other: BuildRequest = { kind: 'addon', mod: 'CADNavigationClient', addon: 'Scripts' };

  assert.equal(isSameRequest(SCRIPTS, other), false);
});

test('an addon is named the way the panel named it, and a single-addon mod by the mod alone', () => {
  assert.equal(nameOf(SCRIPTS), 'CADCore\\Scripts');
  assert.equal(nameOf(CORE), 'CADCore');
  assert.equal(nameOf(ALL), 'every addon');
});
