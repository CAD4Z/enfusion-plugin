import assert from 'node:assert/strict';
import { test } from 'node:test';
import {
  BATCH_EVENT_MAX_BYTES,
  TextureBatchProtocolReader,
} from '../../../src/mods/texture/textureBatchProtocol';

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

test('every split of one stream frames the same events, and every mutation of it is refused', () => {
  const stream = [
    event({ kind: 'batch-started', jobCount: 2 }),
    event({ kind: 'progress', id: 'alpha', progress: 0.25 }),
    event({ kind: 'diagnostic', id: 'alpha', category: 'cancelled', code: 'c', message: 'm' }),
    event({ kind: 'result', id: 'alpha', status: 'Cancelled', reason: 'stopped', retryable: false }),
    event({ kind: 'complete', converted: 0, failed: 0, cancelled: 1 }),
  ].join('\n') + '\n';
  const expected = ['batch-started', 'progress', 'diagnostic', 'result', 'complete'];

  // Where a pipe happened to break is not a protocol fact, so no split may change the events.
  let random = 0x9e3779b9;
  const next = (bound: number): number => {
    random = (random * 1103515245 + 12345) & 0x7fffffff;
    return random % bound + 1;
  };
  for (let round = 0; round < 200; round += 1) {
    const reader = new TextureBatchProtocolReader();
    const framed: string[] = [];
    for (let at = 0; at < stream.length;) {
      const size = next(9);
      framed.push(...reader.push(stream.slice(at, at + size)).map(({ kind }) => kind));
      at += size;
    }
    framed.push(...reader.finish().map(({ kind }) => kind));
    assert.deepEqual(framed, expected);
  }

  // Every single-character edit of a well-formed stream is either refused or still well formed.
  for (let at = 0; at < stream.length; at += 1) {
    for (const replacement of ['', '"', '}', '0', '\n']) {
      const mutated = stream.slice(0, at) + replacement + stream.slice(at + 1);
      const reader = new TextureBatchProtocolReader();
      try {
        const framed = [...reader.push(mutated), ...reader.finish()];
        for (const value of framed) {
          assert.equal(value.protocolVersion, 1);
          assert.equal(expected.includes(value.kind), true);
        }
      } catch (error: unknown) {
        assert.equal(error instanceof Error, true);
      }
    }
  }
});

function event(value: Record<string, unknown>): string {
  return JSON.stringify({ protocolVersion: 1, ...value });
}
