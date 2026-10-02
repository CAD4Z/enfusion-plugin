import { spawn } from 'node:child_process';
import { readdir, unlink } from 'node:fs/promises';
import { unlinkSync, writeFileSync } from 'node:fs';
import { randomUUID } from 'node:crypto';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { EDDS_AREA } from '../mods/edds';
import type { TextureBatchEvent } from '../mods/textureBatchProtocol';
import {
  BATCH_MAX_JOBS,
  BATCH_PROTOCOL_VERSION,
  TextureBatchProtocolReader,
} from '../mods/textureBatchProtocol';
import type { TextureBatchJob } from '../mods/textureBatch';

export const EDDS_BATCH_MAX_JOBS = BATCH_MAX_JOBS;
export const EDDS_BATCH_CANCEL_GRACE_MS = 2_000;
const STDERR_MAX_BYTES = 1024 * 1024;
/** The one exit category the converter uses for a failure that was not the input's fault. */
const EDDS_INTERNAL_FAILURE_EXIT = 6;

/**
 * A batch that never became per-item results. `retryable` separates a process that died from an
 * input the converter refuses identically every time, so Retry Failed cannot loop on the latter.
 */
export class BatchFailure extends Error {
  constructor(message: string, readonly retryable: boolean) {
    super(message);
    this.name = 'BatchFailure';
  }
}

/** A failure the caller did not classify is treated as the input's fault, never as transient. */
export function batchFailureRetryable(error: unknown): boolean {
  return error instanceof BatchFailure && error.retryable;
}

export interface BatchProcessRequest {
  readonly executable: string;
  readonly args: readonly string[];
  readonly shell: false;
  readonly windowsHide: true;
}

export interface BatchProcess {
  readonly completed: Promise<{ readonly code: number | null; readonly signal: string | null }>;
  readonly tempOwner?: string;
  write(value: string): void;
  end(): void;
  onStdout(listener: (chunk: string) => void): void;
  onStderr(listener: (chunk: string) => void): void;
  interrupt(): void;
  kill(): void;
}

export type SpawnBatchProcess = (request: BatchProcessRequest) => BatchProcess;

export interface BatchClock {
  setTimeout(callback: () => void, milliseconds: number): unknown;
  clearTimeout(handle: unknown): void;
}

export interface EddsBatchExecution {
  readonly results: readonly Extract<TextureBatchEvent, { kind: 'result' }>[];
  readonly diagnostics: readonly Extract<TextureBatchEvent, { kind: 'diagnostic' }>[];
  readonly complete: Extract<TextureBatchEvent, { kind: 'complete' }>;
}

export type CleanupBatchTemps = (
  jobs: readonly TextureBatchJob[],
  tempOwner?: string,
) => Promise<void>;

export async function runEddsBatch(
  executable: string,
  jobs: readonly TextureBatchJob[],
  spawnProcess: SpawnBatchProcess = spawnNativeBatch,
  clock: BatchClock = systemClock,
  onEvent: (event: TextureBatchEvent) => void = () => undefined,
  signal?: AbortSignal,
  cleanup: CleanupBatchTemps = cleanupBatchTemps,
): Promise<EddsBatchExecution> {
  if (jobs.length === 0 || jobs.length > EDDS_BATCH_MAX_JOBS) {
    throw new BatchFailure(
      `A conversion batch must contain at least one and at most ${EDDS_BATCH_MAX_JOBS} jobs.`,
      false,
    );
  }
  const identifiers = new Set<string>();
  for (const job of jobs) {
    if (job.id.length === 0 || identifiers.has(job.id)) {
      throw new BatchFailure('Every conversion batch job requires a unique non-empty id.', false);
    }
    identifiers.add(job.id);
  }

  const child = spawnProcess({
    executable,
    args: [EDDS_AREA, 'batch', '--machine', '--protocol', String(BATCH_PROTOCOL_VERSION)],
    shell: false,
    windowsHide: true,
  });
  const reader = new TextureBatchProtocolReader();
  const events: TextureBatchEvent[] = [];
  let stderr = '';
  let protocolFailure: unknown;
  let forced: unknown;
  let cancelled = signal?.aborted === true;
  let cleanExit = false;
  const accept = (event: TextureBatchEvent): void => {
    if ('id' in event && !identifiers.has(event.id)) {
      throw new BatchFailure(`The native batch stream named unknown job ${event.id}.`, false);
    }
    events.push(event);
    if (event.kind !== 'complete') onEvent(event);
  };
  child.onStdout((chunk) => {
    if (protocolFailure !== undefined) return;
    try {
      for (const event of reader.push(chunk)) accept(event);
    } catch (error: unknown) {
      protocolFailure = error;
      child.kill();
    }
  });
  child.onStderr((chunk) => {
    if (Buffer.byteLength(stderr, 'utf8') < STDERR_MAX_BYTES) stderr += chunk;
  });
  const abort = (): void => {
    if (cancelled && forced !== undefined) return;
    cancelled = true;
    child.interrupt();
    forced = clock.setTimeout(() => child.kill(), EDDS_BATCH_CANCEL_GRACE_MS);
  };
  signal?.addEventListener('abort', abort, { once: true });
  if (cancelled) abort();

  try {
    child.write(batchInputOf(jobs));
    child.end();
    const exit = await child.completed;
    if (forced !== undefined) clock.clearTimeout(forced);
    if (protocolFailure === undefined) {
      try {
        for (const event of reader.finish()) accept(event);
      } catch (error: unknown) {
        protocolFailure = error;
      }
    }
    if (protocolFailure !== undefined) throw errorOf(protocolFailure);
    if (cancelled) throw new BatchFailure('The texture conversion batch was cancelled.', false);
    if (exit.code !== 0) {
      /*
       * A process that died, or failed inside itself, is worth the same jobs again. The refusal
       * categories are not: 2, 3 and 4 are this exact input being turned away, and would land on
       * the same code every time it was sent back.
       */
      throw new BatchFailure(
        stderr.trim() || `The native batch process exited with code ${String(exit.code)}.`,
        exit.code === null || exit.code === EDDS_INTERNAL_FAILURE_EXIT,
      );
    }
    const started = events.filter((event) => event.kind === 'batch-started');
    const complete = events.filter((event) => event.kind === 'complete');
    const completed = complete[0];
    if (started.length !== 1 || started[0]?.jobCount !== jobs.length ||
        complete.length !== 1 || completed === undefined) {
      throw new BatchFailure('The native batch stream ended without one matching start and completion event.', false);
    }
    const results = events.filter(
      (event): event is Extract<TextureBatchEvent, { kind: 'result' }> => event.kind === 'result',
    );
    const resultIds = new Set(results.map(({ id }) => id));
    if (results.length !== jobs.length || resultIds.size !== jobs.length ||
        results.some((result) => !identifiers.has(result.id))) {
      throw new BatchFailure('The native batch stream did not return exactly one result for every job.', false);
    }
    const converted = results.filter(({ status }) => status === 'Converted').length;
    const failed = results.filter(({ status }) => status === 'Failed').length;
    const cancelledCount = results.filter(({ status }) => status === 'Cancelled').length;
    if (completed.converted !== converted || completed.failed !== failed ||
        completed.cancelled !== cancelledCount || converted + failed + cancelledCount !== jobs.length) {
      throw new BatchFailure('The native batch completion counts do not match its per-item results.', false);
    }
    const execution = {
      results: jobs.map((job) => {
        const result = results.find(({ id }) => id === job.id);
        if (result === undefined) throw new BatchFailure(`The native batch omitted job ${job.id}.`, false);
        return result;
      }),
      diagnostics: events.filter(
        (event): event is Extract<TextureBatchEvent, { kind: 'diagnostic' }> =>
          event.kind === 'diagnostic',
      ),
      complete: completed,
    };
    onEvent(completed);
    cleanExit = true;
    return execution;
  } finally {
    signal?.removeEventListener('abort', abort);
    if (forced !== undefined) clock.clearTimeout(forced);
    if (!cleanExit) await cleanup(jobs, child.tempOwner).catch(() => undefined);
  }
}

function errorOf(value: unknown): Error {
  return value instanceof Error ? value : new Error(String(value));
}

export function batchInputOf(jobs: readonly TextureBatchJob[]): string {
  const values: Record<string, unknown>[] = [
    { protocolVersion: BATCH_PROTOCOL_VERSION, kind: 'batch', jobCount: jobs.length },
  ];
  for (const { id, plan } of jobs) {
    values.push({
      protocolVersion: BATCH_PROTOCOL_VERSION,
      kind: 'job',
      id,
      input: plan.source,
      output: plan.output,
      metadata: plan.metadata ?? null,
      identity: plan.identity ?? null,
      profile: plan.profile,
      expected: {
        source: revisionText(plan.revisions.source),
        output: revisionText(plan.revisions.output),
        metadata: revisionText(plan.revisions.metadata),
      },
    });
  }
  values.push({ protocolVersion: BATCH_PROTOCOL_VERSION, kind: 'end' });
  return values.map((value) => JSON.stringify(value)).join('\n') + '\n';
}

function revisionText(revision: { readonly size: number; readonly modified: number } | undefined): string {
  return revision === undefined ? 'missing' : `${revision.size}:${revision.modified}`;
}

function spawnNativeBatch(request: BatchProcessRequest): BatchProcess {
  const cancelFile = path.join(tmpdir(), `enfusion-cancel-${randomUUID()}`);
  const child = spawn(request.executable, [...request.args, '--cancel-file', cancelFile], {
    shell: request.shell,
    windowsHide: request.windowsHide,
    stdio: ['pipe', 'pipe', 'pipe'],
  });
  child.stdout.setEncoding('utf8');
  child.stderr.setEncoding('utf8');
  const cleanupCancel = (): void => {
    try { unlinkSync(cancelFile); } catch { /* It normally does not exist. */ }
  };
  return {
    tempOwner: child.pid === undefined ? undefined : String(child.pid),
    completed: new Promise((resolve, reject) => {
      child.once('error', (error) => { cleanupCancel(); reject(error); });
      child.once('close', (code, closedSignal) => {
        cleanupCancel();
        resolve({ code, signal: closedSignal });
      });
    }),
    write(value) { child.stdin.write(value, 'utf8'); },
    end() { child.stdin.end(); },
    onStdout(listener) { child.stdout.on('data', listener); },
    onStderr(listener) { child.stderr.on('data', listener); },
    interrupt() {
      try { writeFileSync(cancelFile, '', { flag: 'wx' }); } catch { /* First marker wins. */ }
    },
    kill() { child.kill('SIGKILL'); },
  };
}

const systemClock: BatchClock = {
  setTimeout(callback, milliseconds) { return setTimeout(callback, milliseconds); },
  clearTimeout(handle) { clearTimeout(handle as NodeJS.Timeout); },
};

async function cleanupBatchTemps(
  jobs: readonly TextureBatchJob[],
  tempOwner?: string,
): Promise<void> {
  if (tempOwner === undefined) return;
  const targets = new Set(jobs.flatMap(({ plan }) => [
    plan.output,
    ...(plan.metadata === undefined ? [] : [plan.metadata]),
  ]));
  await Promise.all([...targets].map(async (target) => {
    const folder = path.dirname(target);
    const prefix = `${path.basename(target)}.enfusion-`;
    const ownedPrefixes = [`${prefix}new-${tempOwner}-`, `${prefix}old-${tempOwner}-`];
    let entries;
    try {
      entries = await readdir(folder, { withFileTypes: true });
    } catch {
      return;
    }
    await Promise.all(entries
      .filter((entry) => entry.isFile() && entry.name.endsWith('.tmp') &&
        ownedPrefixes.some((owned) => entry.name.startsWith(owned)))
      .map((entry) => unlink(path.join(folder, entry.name)).catch(() => undefined)));
  }));
}
