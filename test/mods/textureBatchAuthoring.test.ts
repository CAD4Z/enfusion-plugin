import assert from 'node:assert/strict';
import { test } from 'node:test';
import type { EddsConversion } from '../../src/mods/edds';
import {
  openedTextureBatch,
  updateTextureBatch,
} from '../../src/mods/textureBatchAuthoring';
import { textureBatchPlanOf } from '../../src/mods/textureBatch';
import { DEFAULT_TEXTURE_PROFILE } from '../../src/mods/textureConversion';

test('loading chooses one active viewport and changing rows never changes the common profile', () => {
  const plan = readyPlan();
  const loaded = updateTextureBatch(openedTextureBatch().state, { kind: 'loaded', plan });
  assert.equal(loaded.state.kind, 'authoring');
  assert.equal(loaded.state.kind === 'authoring' && loaded.state.activeSource, 'C:\\mod\\Mod\\alpha.tga');
  assert.deepEqual(loaded.effects, [{
    kind: 'render-item', source: 'C:\\mod\\Mod\\alpha.tga', revision: 1,
    plan: plan.jobs[0], profile: DEFAULT_TEXTURE_PROFILE,
  }]);

  const selected = updateTextureBatch(loaded.state, {
    kind: 'select-item', source: 'C:\\mod\\Mod\\zeta.png',
  });
  assert.equal(selected.state.kind === 'authoring' && selected.state.activeSource, 'C:\\mod\\Mod\\zeta.png');
  assert.strictEqual(selected.state.kind === 'authoring' && selected.state.plan, plan);
  assert.strictEqual(selected.state.kind === 'authoring' && selected.state.draft, plan.profile);
  assert.equal(selected.effects.length, 1);
});

test('one profile edit replaces every ready item and starts only one active-row preview', () => {
  const loaded = updateTextureBatch(openedTextureBatch().state, { kind: 'loaded', plan: readyPlan() });
  const changed = updateTextureBatch(loaded.state, {
    kind: 'change-profile', field: 'FormatCompress', value: 'Best',
  });

  assert.equal(changed.state.kind, 'authoring');
  if (changed.state.kind !== 'authoring') return;
  assert.equal(changed.state.draft.FormatCompress, 'Best');
  const draft = changed.state.draft;
  assert.equal(changed.state.plan.jobs.every((job) => job.profile === draft), true);
  assert.deepEqual(changed.effects.map(({ kind }) => kind), ['render-item']);
});

test('per-item failures do not stop successes and cancellation preserves completed outputs', () => {
  const loaded = updateTextureBatch(openedTextureBatch().state, { kind: 'loaded', plan: readyPlan() });
  const running = updateTextureBatch(loaded.state, { kind: 'run' });
  assert.equal(running.state.kind, 'running');
  assert.equal(running.effects[0]?.kind, 'convert-batch');

  const progressed = updateTextureBatch(running.state, {
    kind: 'native-event', event: { protocolVersion: 1, kind: 'progress', id: '0', progress: 0.5 },
  });
  const converted = updateTextureBatch(progressed.state, {
    kind: 'native-event', event: {
      protocolVersion: 1, kind: 'result', id: '0', status: 'Converted', conversion: CONVERSION,
    },
  });
  const failed = updateTextureBatch(converted.state, {
    kind: 'native-event', event: {
      protocolVersion: 1, kind: 'result', id: '1', status: 'Failed', reason: 'locked', retryable: true,
    },
  });
  const cancelled = updateTextureBatch(failed.state, { kind: 'cancel' });

  assert.equal(cancelled.effects[0]?.kind, 'cancel-batch');
  assert.deepEqual(
    cancelled.state.kind === 'running' && cancelled.state.items.map(({ status }) => status),
    ['Converted', 'Failed'],
  );
  const stopped = updateTextureBatch(cancelled.state, { kind: 'batch-failed', reason: 'cancelled' });
  assert.deepEqual(
    stopped.state.kind === 'result' && stopped.state.items.map(({ status }) => status),
    ['Converted', 'Failed'],
  );
});

test('a crashed process makes unfinished work retryable failures', () => {
  const loaded = updateTextureBatch(openedTextureBatch().state, { kind: 'loaded', plan: readyPlan() });
  const running = updateTextureBatch(loaded.state, { kind: 'run' });

  const crashed = updateTextureBatch(running.state, { kind: 'batch-failed', reason: 'process crashed' });
  assert.deepEqual(
    crashed.state.kind === 'result' && crashed.state.items.map(({ status, retryable }) => [status, retryable]),
    [['Failed', true], ['Failed', true]],
  );
  const retry = updateTextureBatch(crashed.state, { kind: 'retry-failed' });
  assert.equal(retry.state.kind, 'result');
  assert.equal(retry.effects[0]?.kind, 'refresh-retry');
  const refreshed = updateTextureBatch(retry.state, { kind: 'retry-refreshed', plan: readyPlan() });
  assert.equal(refreshed.state.kind, 'running');
  assert.deepEqual(
    refreshed.effects[0]?.kind === 'convert-batch' && refreshed.effects[0].jobs.map(({ id }) => id),
    ['0', '1'],
  );
  if (crashed.state.kind === 'result' && refreshed.effects[0]?.kind === 'convert-batch') {
    assert.strictEqual(refreshed.effects[0].jobs[0]?.plan, crashed.state.items[0]?.plan);
  }
});

test('Retry Failed creates a new batch from retryable failures only', () => {
  let state = updateTextureBatch(
    updateTextureBatch(openedTextureBatch().state, { kind: 'loaded', plan: readyPlan() }).state,
    { kind: 'run' },
  ).state;
  state = updateTextureBatch(state, {
    kind: 'native-event', event: {
      protocolVersion: 1, kind: 'result', id: '0', status: 'Failed', reason: 'busy', retryable: true,
    },
  }).state;
  state = updateTextureBatch(state, {
    kind: 'native-event', event: {
      protocolVersion: 1, kind: 'result', id: '1', status: 'Failed', reason: 'invalid', retryable: false,
    },
  }).state;
  state = updateTextureBatch(state, {
    kind: 'native-event', event: {
      protocolVersion: 1, kind: 'complete', converted: 0, failed: 2, cancelled: 0,
    },
  }).state;

  const checking = updateTextureBatch(state, { kind: 'retry-failed' });
  assert.equal(checking.effects[0]?.kind, 'refresh-retry');
  const retry = updateTextureBatch(checking.state, { kind: 'retry-refreshed', plan: readyPlan() });

  assert.equal(retry.state.kind, 'running');
  assert.deepEqual(
    retry.effects[0]?.kind === 'convert-batch' && retry.effects[0].jobs.map(({ id }) => id),
    ['0'],
  );
});

test('an invalid input is retried only after a refreshed revision proves its cause changed', () => {
  let state = updateTextureBatch(
    updateTextureBatch(openedTextureBatch().state, { kind: 'loaded', plan: readyPlan() }).state,
    { kind: 'run' },
  ).state;
  state = updateTextureBatch(state, {
    kind: 'native-event', event: {
      protocolVersion: 1, kind: 'result', id: '0', status: 'Failed', reason: 'invalid', retryable: false,
    },
  }).state;
  state = updateTextureBatch(state, {
    kind: 'native-event', event: {
      protocolVersion: 1, kind: 'result', id: '1', status: 'Converted', conversion: CONVERSION,
    },
  }).state;
  state = updateTextureBatch(state, {
    kind: 'native-event', event: {
      protocolVersion: 1, kind: 'complete', converted: 1, failed: 1, cancelled: 0,
    },
  }).state;

  const unchangedCheck = updateTextureBatch(state, { kind: 'retry-failed' });
  const unchanged = updateTextureBatch(unchangedCheck.state, {
    kind: 'retry-refreshed', plan: readyPlan(),
  });
  assert.equal(unchanged.state.kind, 'result');
  assert.deepEqual(unchanged.effects, []);

  const changedCheck = updateTextureBatch(unchanged.state, { kind: 'retry-failed' });
  const changed = updateTextureBatch(changedCheck.state, {
    kind: 'retry-refreshed', plan: readyPlan(3),
  });
  assert.equal(changed.state.kind, 'running');
  assert.deepEqual(
    changed.effects[0]?.kind === 'convert-batch' && changed.effects[0].jobs.map(({ id }) => id),
    ['0'],
  );
});

function readyPlan(alphaModified = 2) {
  const plan = textureBatchPlanOf({
    primary: 'C:\\mod\\Mod\\alpha.tga',
    roots: [{ root: 'C:\\mod', prefixRoot: 'C:\\mod\\Mod' }],
    occupiedGuids: [],
    items: [
      item('C:\\mod\\Mod\\zeta.png', '0123456789ABCDEF'),
      item('C:\\mod\\Mod\\alpha.tga', '1023456789ABCDEF', alphaModified),
    ],
  });
  if (plan.kind !== 'ready') throw new Error(plan.reason);
  return plan;
}

function item(source: string, newGuid: string, modified = 2) {
  return {
    source,
    kind: 'file' as const,
    sourceRevision: { size: 1, modified },
    outputRevision: undefined,
    metadata: { kind: 'missing' as const },
    newGuid,
  };
}

const CONVERSION: EddsConversion = {
  width: 1, height: 1, mipCount: 1, pixelFormat: 'BGRA8', registered: true,
};
