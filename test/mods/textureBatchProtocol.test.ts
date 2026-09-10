import assert from 'node:assert/strict';
import { test } from 'node:test';
import {
  BATCH_EVENT_MAX_BYTES,
  TextureBatchProtocolReader,
} from '../../src/mods/textureBatchProtocol';

test('batch events survive arbitrary stdout chunking', () => {
  const reader = new TextureBatchProtocolReader();
  const source = [
    event({ kind: 'batch-started', jobCount: 2 }),
    event({ kind: 'progress', id: 'alpha', progress: 0.5 }),
    event({ kind: 'diagnostic', id: 'alpha', category: 'invalid-input', code: 'bad-png', message: 'bad' }),
    event({ kind: 'result', id: 'alpha', status: 'Failed', reason: 'bad', retryable: false }),
    event({ kind: 'result', id: 'beta', status: 'Converted', conversion: {
      width: 2, height: 1, mipCount: 1, pixelFormat: 'BGRX8', registered: false,
    } }),
    event({ kind: 'complete', converted: 1, failed: 1, cancelled: 0 }),
  ].join('\n') + '\n';

  const events = [];
  for (let at = 0; at < source.length; at += (at % 7) + 1) {
    events.push(...reader.push(source.slice(at, at + (at % 7) + 1)));
  }
  reader.finish();

  assert.deepEqual(events.map(({ kind }) => kind), [
    'batch-started', 'progress', 'diagnostic', 'result', 'result', 'complete',
  ]);
});

test('malformed, unknown, incompatible and oversized events fail predictably', () => {
  for (const [line, reason] of [
    ['{broken}\n', /malformed JSON/],
    [event({ kind: 'surprise' }) + '\n', /unknown batch event/],
    [JSON.stringify({ protocolVersion: 2, kind: 'complete' }) + '\n', /protocol version 2/],
  ] as const) {
    const reader = new TextureBatchProtocolReader();
    assert.throws(() => reader.push(line), reason);
  }

  const oversized = new TextureBatchProtocolReader();
  assert.throws(() => oversized.push('x'.repeat(BATCH_EVENT_MAX_BYTES + 1)), /exceeds/);
});

test('an unterminated final event is accepted but trailing partial JSON is not', () => {
  const complete = new TextureBatchProtocolReader();
  complete.push(event({ kind: 'complete', converted: 0, failed: 0, cancelled: 0 }));
  assert.deepEqual(complete.finish(), [
    { protocolVersion: 1, kind: 'complete', converted: 0, failed: 0, cancelled: 0 },
  ]);

  const broken = new TextureBatchProtocolReader();
  broken.push('{');
  assert.throws(() => broken.finish(), /malformed JSON/);
});

function event(value: Record<string, unknown>): string {
  return JSON.stringify({ protocolVersion: 1, ...value });
}
