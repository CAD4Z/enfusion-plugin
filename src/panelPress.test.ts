import assert from 'node:assert/strict';
import { test } from 'node:test';
import { STOLEN_PRESS_MS, isStolenPress } from './webview/press';

test('a real pointer click is accepted immediately after the panel regains focus', () => {
  assert.equal(isStolenPress(1, 50), false);
});

test('a synthetic click returned with focus is ignored only inside the settle window', () => {
  assert.equal(isStolenPress(0, 50), true);
  assert.equal(isStolenPress(0, STOLEN_PRESS_MS), false);
});
