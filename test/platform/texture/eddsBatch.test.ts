import assert from 'node:assert/strict';
import { mkdir, mkdtemp, readFile, readdir, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { test } from 'node:test';
import type { TextureBatchJob } from '../../../src/mods/texture/textureBatch';
import type { TextureBatchEvent } from '../../../src/mods/texture/textureBatchProtocol';
import { DEFAULT_TEXTURE_PROFILE } from '../../../src/mods/texture/textureConversion';
import {
  EDDS_BATCH_MAX_JOBS,
  batchFailureRetryable,
  type BatchClock,
  type BatchProcess,
  type BatchProcessRequest,
  runEddsBatch,
} from '../../../src/platform/texture/eddsBatch';

test('one batch request owns one process and reads fragmented events', async () => {
  const child = new FakeBatchProcess();
  const requests: BatchProcessRequest[] = [];
  const seen: TextureBatchEvent[] = [];
  const running = runEddsBatch(
    'C:\\extension\\enfusion.exe',
    [plan('alpha', 'C:\\mod\\alpha.png'), plan('beta', 'C:\\mod\\beta.tga')],
    (request) => { requests.push(request); return child; },
    realClock,
    (event) => seen.push(event),
  );

  assert.equal(requests.length, 1);
  assert.deepEqual(requests[0], {
    executable: 'C:\\extension\\enfusion.exe',
    args: ['edds', 'batch', '--machine', '--protocol', '1'],
    shell: false,
    windowsHide: true,
  });
  const input = child.input.trim().split('\n').map(recordOf);
  assert.equal(input[0].kind, 'batch');
  assert.equal(input[0].jobCount, 2);
  assert.deepEqual(input.slice(1, 3).map(({ id, kind }) => [id, kind]), [
    ['alpha', 'job'], ['beta', 'job'],
  ]);
  assert.equal(input[3].kind, 'end');

  const stdout = [
    event({ kind: 'batch-started', jobCount: 2 }),
    event({ kind: 'progress', id: 'alpha', progress: 1 }),
    event({ kind: 'result', id: 'alpha', status: 'Converted', conversion: {
      width: 1, height: 1, mipCount: 1, pixelFormat: 'BGRA8', registered: true,
    } }),
    event({ kind: 'result', id: 'beta', status: 'Failed', reason: 'bad input', retryable: false }),
    event({ kind: 'complete', converted: 1, failed: 1, cancelled: 0 }),
  ].join('\n') + '\n';
  child.stdout(stdout.slice(0, 17));
  child.stdout(stdout.slice(17, 83));
  child.stdout(stdout.slice(83));
  child.close(0);

  const completed = await running;
  assert.equal(child.closedInput, true);
  assert.deepEqual(seen.map(({ kind }) => kind), [
    'batch-started', 'progress', 'result', 'result', 'complete',
  ]);
  assert.deepEqual(completed.results.map(({ id, status }) => [id, status]), [
    ['alpha', 'Converted'], ['beta', 'Failed'],
  ]);
});

test('cancellation interrupts first and force-kills only after the grace period', async () => {
  const child = new FakeBatchProcess();
  let forced: (() => void) | undefined;
  const clock: BatchClock = {
    setTimeout(callback) { forced = callback; return 1; },
    clearTimeout() { forced = undefined; },
  };
  const controller = new AbortController();
  const running = runEddsBatch(
    'enfusion.exe',
    [plan('alpha', 'C:\\mod\\alpha.png')],
    () => child,
    clock,
    () => undefined,
    controller.signal,
  );

  controller.abort();
  assert.equal(child.interrupted, 1);
  assert.equal(child.killed, 0);
  forced?.();
  assert.equal(child.killed, 1);
  child.close(5);
  await assert.rejects(running, /cancelled/);
});

test('the documented hard limit refuses input before a process is spawned', async () => {
  let spawned = false;
  await assert.rejects(
    runEddsBatch(
      'enfusion.exe',
      Array.from({ length: EDDS_BATCH_MAX_JOBS + 1 }, (_, at) => plan(String(at), `C:\\mod\\${at}.png`)),
      () => { spawned = true; return new FakeBatchProcess(); },
      realClock,
      () => undefined,
    ),
    /at most/,
  );
  assert.equal(spawned, false);
});

test('a crashed or force-killed batch asks the filesystem boundary to clean job temps', async () => {
  const child = new FakeBatchProcess();
  const cleaned: string[][] = [];
  const running = runEddsBatch(
    'enfusion.exe',
    [plan('alpha', 'C:\\mod\\alpha.png')],
    () => child,
    realClock,
    () => undefined,
    undefined,
    (jobs) => {
      cleaned.push(jobs.flatMap(({ plan: item }) => [item.output, item.metadata ?? '']));
      return Promise.resolve();
    },
  );
  child.close(6);

  await assert.rejects(running, /code 6/);
  assert.deepEqual(cleaned, [[
    'C:\\mod\\alpha.edds',
    'C:\\mod\\alpha.edds.meta',
  ]]);
});

test('after a crash the default cleanup removes only the temps the dead process named', async () => {
  const folder = await mkdtemp(path.join(tmpdir(), 'enfusion-batch-'));
  try {
    const job = plan('alpha', path.join(folder, 'alpha.png'));
    const owned = [
      `${job.plan.output}.enfusion-new-4242-0.tmp`,
      `${job.plan.metadata}.enfusion-new-4242-1.tmp`,
    ];
    const foreign = `${job.plan.output}.enfusion-new-9999-0.tmp`;
    for (const file of [...owned, foreign]) await writeFile(file, '');
    const child = new FakeBatchProcess('4242');
    const running = runEddsBatch('enfusion.exe', [job], () => child, realClock);
    child.close(6);

    await assert.rejects(running, /code 6/);
    assert.deepEqual(await readdir(folder), [path.basename(foreign)]);
  } finally {
    await rm(folder, { recursive: true, force: true });
  }
});

test('only a death or an internal failure comes back as work worth sending again', async () => {
  // The converter's own exit categories: 2, 3 and 4 turn this exact input away every time.
  for (const [code, retryable] of [
    [2, false], [3, false], [4, false], [6, true], [null, true],
  ] as const) {
    const child = new FakeBatchProcess();
    const running = runEddsBatch(
      'enfusion.exe',
      [plan('alpha', 'C:\\mod\\alpha.png')],
      () => child,
      realClock,
      () => undefined,
      undefined,
      () => Promise.resolve(),
    );
    child.close(code);
    const failure = await running.then(() => undefined, (error: unknown) => error);
    assert.equal(batchFailureRetryable(failure), retryable);
  }

  const oversized = runEddsBatch(
    'enfusion.exe',
    Array.from({ length: EDDS_BATCH_MAX_JOBS + 1 }, (_, at) => plan(String(at), `C:\\mod\\t${at}.png`)),
    () => new FakeBatchProcess(),
    realClock,
  );
  assert.equal(batchFailureRetryable(await oversized.catch((error: unknown) => error)), false);
});

test('completion counters must agree with the per-item results', async () => {
  const child = new FakeBatchProcess();
  const seen: TextureBatchEvent[] = [];
  const running = runEddsBatch(
    'enfusion.exe',
    [plan('alpha', 'C:\\mod\\alpha.png')],
    () => child,
    realClock,
    (value) => seen.push(value),
  );
  child.stdout([
    event({ kind: 'batch-started', jobCount: 1 }),
    event({ kind: 'result', id: 'alpha', status: 'Converted', conversion: {
      width: 1, height: 1, mipCount: 1, pixelFormat: 'BGRA8', registered: true,
    } }),
    event({ kind: 'complete', converted: 0, failed: 0, cancelled: 0 }),
  ].join('\n') + '\n');
  child.close(0);

  await assert.rejects(running, /completion counts/);
  assert.equal(seen.some(({ kind }) => kind === 'complete'), false);
});

for (const stage of ['backed-up', 'output-published', 'pair-published', 'committed', 'backup-cleaned'] as const) {
  test(`crash recovery preserves a complete pair after ${stage}`, async (context) => {
    const folder = await mkdtemp(path.join(tmpdir(), 'enfusion-recovery-'));
    context.after(async () => {
      assert.equal(path.dirname(path.resolve(folder)), path.resolve(tmpdir()));
      await rm(folder, { recursive: true, force: true });
    });
    const draft = plan('texture', path.join(folder, 'texture.png'));
    const job: TextureBatchJob = { ...draft, plan: { ...draft.plan, revisions: {
      source: { size: 1, modified: 2 }, output: { size: 10, modified: 2 }, metadata: { size: 10, modified: 2 },
    } } };
    const output = job.plan.output;
    const metadata = job.plan.metadata!;
    const done = stage === 'committed' || stage === 'backup-cleaned';
    const suffix = '.enfusion-';
    await writeFile(output + suffix + (done ? 'committed' : 'pending') + '-4242-0.tmp', '');
    if (stage !== 'backup-cleaned') await writeFile(output + suffix + 'old-4242-0.tmp', 'old texture');
    await writeFile(metadata + suffix + 'old-4242-0.tmp', 'old identity');
    await writeFile(output + suffix + 'old-9999-0.tmp', 'another process');
    await writeFile(stage === 'backed-up' ? output + suffix + 'new-4242-0.tmp' : output, 'new texture');
    await writeFile(stage === 'backed-up' || stage === 'output-published'
      ? metadata + suffix + 'new-4242-0.tmp' : metadata, 'new identity');
    const child = new FakeBatchProcess('4242');
    const running = runEddsBatch('unused.exe', [job], () => child);
    child.close(6);
    await assert.rejects(running);
    assert.equal(await readFile(output, 'utf8'), done ? 'new texture' : 'old texture');
    assert.equal(await readFile(metadata, 'utf8'), done ? 'new identity' : 'old identity');
    assert.deepEqual((await readdir(folder)).sort(), [
      path.basename(output), path.basename(output) + suffix + 'old-9999-0.tmp', path.basename(metadata),
    ].sort());
  });
}

test('crash recovery removes a half-published new pair and restores backups from older converters', async (context) => {
  const folder = await mkdtemp(path.join(tmpdir(), 'enfusion-recovery-'));
  context.after(async () => {
    assert.equal(path.dirname(path.resolve(folder)), path.resolve(tmpdir()));
    await rm(folder, { recursive: true, force: true });
  });
  const created = plan('new', path.join(folder, 'new.png'));
  await writeFile(created.plan.output, 'half-published texture');
  await writeFile(created.plan.output + '.enfusion-pending-4242-0.tmp', '');
  await writeFile(created.plan.metadata + '.enfusion-new-4242-0.tmp', 'unpublished identity');
  const previous = plan('old', path.join(folder, 'old.png'));
  await writeFile(previous.plan.output + '.enfusion-old-4242-0.tmp', 'old texture');
  await writeFile(previous.plan.metadata + '.enfusion-old-4242-0.tmp', 'old identity');
  const child = new FakeBatchProcess('4242');
  const running = runEddsBatch('unused.exe', [created, previous], () => child);
  child.close(null);
  await assert.rejects(running);
  assert.deepEqual((await readdir(folder)).sort(), ['old.edds', 'old.edds.meta']);
  assert.equal(await readFile(previous.plan.output, 'utf8'), 'old texture');
  assert.equal(await readFile(previous.plan.metadata, 'utf8'), 'old identity');
});

test('a failed restore retains both backups and reports recovery failure without retrying', async (context) => {
  const folder = await mkdtemp(path.join(tmpdir(), 'enfusion-recovery-'));
  context.after(async () => {
    assert.equal(path.dirname(path.resolve(folder)), path.resolve(tmpdir()));
    await rm(folder, { recursive: true, force: true });
  });
  const job = plan('texture', path.join(folder, 'texture.png'));
  const backup = '.enfusion-old-4242-0.tmp';
  const marker = job.plan.output + '.enfusion-pending-4242-0.tmp';
  await writeFile(job.plan.output + backup, 'old texture');
  await writeFile(job.plan.metadata + backup, 'old identity');
  await writeFile(marker, '');
  await mkdir(job.plan.output);
  const child = new FakeBatchProcess('4242');
  const running = runEddsBatch('unused.exe', [job], () => child);
  child.close(6);
  const failure: unknown = await running.catch((error: unknown) => error);
  assert.ok(failure instanceof Error);
  assert.match(failure.message, /recovery could not finish/);
  assert.equal(batchFailureRetryable(failure), false);
  assert.equal(await readFile(job.plan.output + backup, 'utf8'), 'old texture');
  assert.equal(await readFile(job.plan.metadata + backup, 'utf8'), 'old identity');
  assert.equal(await readFile(marker, 'utf8'), '');
});

class FakeBatchProcess implements BatchProcess {
  input = '';
  closedInput = false;
  interrupted = 0;
  killed = 0;
  private stdoutListener: (chunk: string) => void = () => undefined;
  private stderrListener: (chunk: string) => void = () => undefined;
  private resolve!: (exit: { code: number | null; signal: string | null }) => void;
  readonly completed = new Promise<{ code: number | null; signal: string | null }>((resolve) => {
    this.resolve = resolve;
  });

  constructor(readonly tempOwner?: string) {}

  write(value: string): void { this.input += value; }
  end(): void { this.closedInput = true; }
  onStdout(listener: (chunk: string) => void): void { this.stdoutListener = listener; }
  onStderr(listener: (chunk: string) => void): void { this.stderrListener = listener; }
  interrupt(): void { this.interrupted += 1; }
  kill(): void { this.killed += 1; }
  stdout(value: string): void { this.stdoutListener(value); }
  stderr(value: string): void { this.stderrListener(value); }
  close(code: number | null): void { this.resolve({ code, signal: code === null ? 'SIGKILL' : null }); }
}

const realClock: BatchClock = {
  setTimeout(callback, milliseconds) { return setTimeout(callback, milliseconds); },
  clearTimeout(handle) { clearTimeout(handle as NodeJS.Timeout); },
};

function plan(id: string, source: string) {
  return {
    id,
    plan: {
      kind: 'ready' as const,
      scope: 'registered' as const,
      action: 'convert' as const,
      label: 'Convert' as const,
      source,
      sourceFormat: source.endsWith('.png') ? 'PNG' as const : 'TGA' as const,
      output: source.replace(/\.[^.]+$/, '.edds'),
      metadata: `${source.replace(/\.[^.]+$/, '.edds')}.meta`,
      identity: { guid: id.padStart(16, '0'), name: `Mod/${id}.edds`, sourceFile: source.split('\\').at(-1)! },
      identityAction: 'create' as const,
      profile: DEFAULT_TEXTURE_PROFILE,
      revisions: { source: { size: 1, modified: 2 } },
    },
  };
}

function event(value: Record<string, unknown>): string {
  return JSON.stringify({ protocolVersion: 1, ...value });
}

function recordOf(source: string): Record<string, unknown> {
  const value = JSON.parse(source) as unknown;
  assert.ok(typeof value === 'object' && value !== null && !Array.isArray(value));
  return value as Record<string, unknown>;
}
