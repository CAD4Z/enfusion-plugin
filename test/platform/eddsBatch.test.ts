import assert from 'node:assert/strict';
import { test } from 'node:test';
import type { TextureBatchEvent } from '../../src/mods/textureBatchProtocol';
import { DEFAULT_TEXTURE_PROFILE } from '../../src/mods/textureConversion';
import {
  EDDS_BATCH_MAX_JOBS,
  batchFailureRetryable,
  type BatchClock,
  type BatchProcess,
  type BatchProcessRequest,
  runEddsBatch,
} from '../../src/platform/eddsBatch';

test('one batch request owns one process and reads fragmented events', async () => {
  const child = new FakeBatchProcess();
  const requests: BatchProcessRequest[] = [];
  const seen: TextureBatchEvent[] = [];
  const running = runEddsBatch(
    'C:\\extension\\edds-convert.exe',
    [plan('alpha', 'C:\\mod\\alpha.png'), plan('beta', 'C:\\mod\\beta.tga')],
    (request) => { requests.push(request); return child; },
    realClock,
    (event) => seen.push(event),
  );

  assert.equal(requests.length, 1);
  assert.deepEqual(requests[0], {
    executable: 'C:\\extension\\edds-convert.exe',
    args: ['batch', '--machine', '--protocol', '1'],
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
    'edds-convert.exe',
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
      'edds-convert.exe',
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
    'edds-convert.exe',
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

test('only a death or an internal failure comes back as work worth sending again', async () => {
  // The converter's own exit categories: 2, 3 and 4 turn this exact input away every time.
  for (const [code, retryable] of [
    [2, false], [3, false], [4, false], [6, true], [null, true],
  ] as const) {
    const child = new FakeBatchProcess();
    const running = runEddsBatch(
      'edds-convert.exe',
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
    'edds-convert.exe',
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
    'edds-convert.exe',
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
