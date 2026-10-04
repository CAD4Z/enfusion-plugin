import { spawn } from 'node:child_process';
import { copyFile, readdir, stat, unlink } from 'node:fs/promises';
import { unlinkSync, writeFileSync } from 'node:fs';
import { randomUUID } from 'node:crypto';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { EDDS_AREA } from '../../mods/texture/edds';
import type { TextureBatchEvent } from '../../mods/texture/textureBatchProtocol';
import {
  BATCH_MAX_JOBS,
  BATCH_PROTOCOL_VERSION,
  NativeBatchError,
  TextureBatchProtocolReader,
} from '../../mods/texture/textureBatchProtocol';
import type { TextureBatchJob } from '../../mods/texture/textureBatch';

export const EDDS_BATCH_MAX_JOBS = BATCH_MAX_JOBS;
export const EDDS_BATCH_CANCEL_GRACE_MS = 2_000;
const STDERR_MAX_BYTES = 1024 * 1024;
/** The exit categories that are this exact input being turned away: invocation, input, format. */
const EDDS_REFUSAL_EXITS: ReadonlySet<number> = new Set([2, 3, 4]);

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
  let exited = false;
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
    exited = true;
    if (forced !== undefined) clock.clearTimeout(forced);
    if (protocolFailure === undefined) {
      try {
        for (const event of reader.finish()) accept(event);
      } catch (error: unknown) {
        protocolFailure = error;
      }
    }
    if (protocolFailure instanceof NativeBatchError) {
      throw new BatchFailure(protocolFailure.message, protocolFailure.category === 'internal-failure');
    }
    if (protocolFailure !== undefined) throw errorOf(protocolFailure);
    if (cancelled) throw new BatchFailure('The texture conversion batch was cancelled.', false);
    if (exit.code !== 0) {
      /*
       * A process that died, crashed, was terminated or failed inside itself is worth the same
       * jobs again. The refusal categories are not: 2, 3 and 4 are this exact input being turned
       * away, and would land on the same code every time it was sent back.
       */
      throw new BatchFailure(
        stderr.trim() || `The native batch process exited with code ${String(exit.code)}.`,
        exit.code === null || !EDDS_REFUSAL_EXITS.has(exit.code),
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
    // A job whose own rollback failed reports Failed and keeps its journal; it is recovered here
    // rather than left in the mod folder. A clean job leaves nothing behind, and nothing is done.
    await recoverBatch(cleanup, jobs, child.tempOwner);
    onEvent(completed);
    return execution;
  } catch (error: unknown) {
    if (!exited) {
      child.kill();
      await child.completed.catch(() => undefined);
    }
    if (!(error instanceof RecoveryFailure)) await recoverBatch(cleanup, jobs, child.tempOwner);
    throw error;
  } finally {
    signal?.removeEventListener('abort', abort);
    if (forced !== undefined) clock.clearTimeout(forced);
  }
}

/** A recovery that could not finish: never retried, and never followed by a second recovery. */
class RecoveryFailure extends BatchFailure {}

async function recoverBatch(
  cleanup: CleanupBatchTemps,
  jobs: readonly TextureBatchJob[],
  tempOwner: string | undefined,
): Promise<void> {
  try {
    await cleanup(jobs, tempOwner);
  } catch (recoveryError: unknown) {
    throw new RecoveryFailure(
      `Batch recovery could not finish; recovery files were retained. ${errorOf(recoveryError).message}`,
      false,
    );
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
  // A converter that refuses the batch exits before it has read every job, and the rest of the
  // write fails with EPIPE; the exit code and stdout say why, so the pipe error says nothing more.
  child.stdin.on('error', () => undefined);
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
  const recovered = await Promise.allSettled(jobs.map(({ plan }) => recoverTexturePair(plan, tempOwner)));
  const failures = recovered.flatMap((result) => result.status === 'rejected'
    ? [errorOf(result.reason).message] : []);
  if (failures.length > 0) throw new Error(failures.join('\n'));
}

/** What recovery needs to know of a conversion: its pair, and whether each member existed. */
export interface PublishedPair {
  readonly output: string;
  readonly metadata?: string;
  readonly revisions: {
    readonly output?: { readonly size: number; readonly modified: number };
    readonly metadata?: { readonly size: number; readonly modified: number };
  };
}

/**
 * Puts back the pair a dead converter process left behind, by the temporaries it named with its
 * own id. A native commit marker covers the pair; an unfinished journal always rolls it back.
 */
export async function recoverTexturePair(plan: PublishedPair, owner: string): Promise<void> {
  const folder = path.dirname(plan.output);
  const entries = await readdir(folder, { withFileTypes: true }).catch((error: unknown) => {
    if ((error as NodeJS.ErrnoException).code === 'ENOENT') return [];
    throw error;
  });
  const owned = (target: string, kind: string): string[] => {
    const prefix = `${path.basename(target)}.enfusion-${kind}-${owner}-`;
    return entries.filter((entry) => entry.isFile() && entry.name.startsWith(prefix) &&
      /^\d+\.tmp$/.test(entry.name.slice(prefix.length)))
      .map((entry) => path.join(folder, entry.name));
  };
  const pending = owned(plan.output, 'pending');
  const committed = owned(plan.output, 'committed');
  const artifacts = [
    { target: plan.output, previous: plan.revisions.output },
    ...(plan.metadata === undefined ? [] : [{ target: plan.metadata, previous: plan.revisions.metadata }]),
  ].map((artifact) => ({ ...artifact, backups: owned(artifact.target, 'old'), drafts: owned(artifact.target, 'new') }));
  if (committed.length > 0) {
    // Even if the process died while reporting the result, the whole published pair is retained.
    for (const artifact of artifacts) await stat(artifact.target);
  } else {
    for (const artifact of artifacts) {
      if (artifact.backups.length > 1) throw new Error(`Ambiguous backups for ${artifact.target}.`);
      const backup = artifact.backups[0];
      if (backup !== undefined) {
        // No journal means an older converter: a present destination is ambiguous, so retain it
        // and the backup for recovery instead of guessing which version belongs to the pair.
        if (pending.length === 0 && await fileExists(artifact.target)) {
          throw new Error(`Unjournalled backup retained at ${backup}.`);
        }
        await copyFile(backup, artifact.target);
      } else if (pending.length > 0) {
        if (artifact.previous === undefined) await removeIfPresent(artifact.target);
        else await stat(artifact.target);
      }
    }
  }
  // Backups survive until every member is recovered. A failed restore cannot destroy the evidence.
  for (const artifact of artifacts) {
    for (const temporary of [...artifact.backups, ...artifact.drafts]) await removeIfPresent(temporary);
  }
  for (const marker of [...pending, ...committed]) await removeIfPresent(marker);
}

async function fileExists(file: string): Promise<boolean> {
  try { await stat(file); return true; } catch (error: unknown) {
    if ((error as NodeJS.ErrnoException).code === 'ENOENT') return false;
    throw error;
  }
}

async function removeIfPresent(file: string): Promise<void> {
  try { await unlink(file); } catch (error: unknown) {
    if ((error as NodeJS.ErrnoException).code !== 'ENOENT') throw error;
  }
}
