import assert from 'node:assert/strict';
import { test } from 'node:test';
import { TextureScheduler } from '../../src/platform/textureScheduler';

test('all texture work shares one slot and heavy work runs before queued preview', async () => {
  const scheduler = new TextureScheduler();
  const order: string[] = [];
  let releaseInspect!: () => void;
  const inspect = scheduler.run('inspect', async () => {
    order.push('inspect:start');
    await new Promise<void>((resolve) => { releaseInspect = resolve; });
    order.push('inspect:end');
    return 'inspect';
  });
  const preview = scheduler.run('preview', () => {
    order.push('preview');
    return Promise.resolve('preview');
  });
  const previewOutcome = preview.then(
    (value) => value,
    () => 'cancelled',
  );
  const convert = scheduler.run('convert', () => {
    order.push('convert');
    return Promise.resolve('convert');
  });

  await Promise.resolve();
  releaseInspect();
  assert.deepEqual(await Promise.all([inspect, previewOutcome, convert]), ['inspect', 'cancelled', 'convert']);
  assert.deepEqual(order, ['inspect:start', 'inspect:end', 'convert']);
});

test('a heavy request cooperatively cancels an active preview before it starts', async () => {
  const scheduler = new TextureScheduler();
  let previewAborted = false;
  const preview = scheduler.run('preview', (signal) => new Promise<string>((_resolve, reject) => {
    signal.addEventListener('abort', () => {
      previewAborted = true;
      reject(signal.reason instanceof Error ? signal.reason : new Error(String(signal.reason)));
    });
  }));
  let queuedRan = false;
  const queued = scheduler.run('preview', () => {
    queuedRan = true;
    return Promise.resolve('queued');
  });
  await Promise.resolve();

  const convert = scheduler.run('batch', () => Promise.resolve('batch'));

  await assert.rejects(preview);
  await assert.rejects(queued);
  assert.equal(previewAborted, true);
  assert.equal(queuedRan, false);
  assert.equal(await convert, 'batch');
});

test('aborting queued preview removes it without consuming the slot', async () => {
  const scheduler = new TextureScheduler();
  let release!: () => void;
  const active = scheduler.run('convert', () => new Promise<void>((resolve) => { release = resolve; }));
  const controller = new AbortController();
  let ran = false;
  const preview = scheduler.run('preview', () => {
    ran = true;
    return Promise.resolve();
  }, controller.signal);

  controller.abort(new Error('superseded'));
  release();

  await active;
  await assert.rejects(preview, /superseded/);
  assert.equal(ran, false);
});
