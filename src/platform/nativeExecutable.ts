/**
 * The one bundled native executable, and how every area of it is run.
 *
 * The path is installation-relative and never configurable. Arguments go straight to the process
 * with no shell, so a path remains one opaque argument. A command that can be asked to stop gets a
 * cancel file: an abort creates it, and the process rolls its work back before it exits.
 */
import { type ChildProcess, execFile } from 'node:child_process';
import { writeFileSync } from 'node:fs';
import path from 'node:path';

/** Where the packaged extension keeps the executable. */
export function bundledExecutable(extensionPath: string): string {
  return path.join(extensionPath, 'dist', 'native', 'win32-x64', 'enfusion.exe');
}

export interface ExecutableRequest {
  readonly executable: string;
  readonly args: readonly string[];
  readonly shell: false;
  readonly windowsHide: true;
  readonly maxBuffer: number;
  readonly signal?: AbortSignal;
  /**
   * The file the process watches for a request to stop, when it has one. An abort then creates it
   * instead of killing the process, so the process can roll its files back first.
   */
  readonly cancelFile?: string;
  /** With a cancel file: how long a process that was asked to stop may take before it is killed. */
  readonly killAfterMs?: number;
}

export interface ExecutableResult {
  readonly stdout: string;
  readonly stderr: string;
}

export type Execute = (request: ExecutableRequest) => Promise<ExecutableResult>;

/**
 * Runs the request. A failure rejects with the child's error, its stdout and stderr, its `pid`, and
 * the name `AbortError` when the caller aborted it.
 */
export function executeFile(request: ExecutableRequest): Promise<ExecutableResult> {
  return new Promise<ExecutableResult>((resolve, reject) => {
    const graceful = request.cancelFile !== undefined;
    let forced: NodeJS.Timeout | undefined;
    let aborted = false;
    const child: ChildProcess = execFile(
      request.executable,
      [...request.args],
      {
        encoding: 'utf8',
        maxBuffer: request.maxBuffer,
        shell: request.shell,
        // A process that can be asked to stop is asked; anything else is simply stopped.
        signal: graceful ? undefined : request.signal,
        windowsHide: request.windowsHide,
      },
      (error, stdout, stderr) => {
        if (forced !== undefined) clearTimeout(forced);
        request.signal?.removeEventListener('abort', abort);
        if (error === null) {
          resolve({ stdout, stderr });
          return;
        }
        reject(Object.assign(new Error(error.message), error, {
          stdout,
          stderr,
          pid: child.pid,
          ...(aborted ? { name: 'AbortError' } : {}),
        }));
      },
    );
    function abort(): void {
      aborted = true;
      try {
        writeFileSync(request.cancelFile ?? '', '', { flag: 'wx' });
      } catch {
        // The first request to stop is the one that counts.
      }
      if (request.killAfterMs !== undefined) forced = setTimeout(() => child.kill('SIGKILL'), request.killAfterMs);
    }
    if (graceful && request.signal !== undefined) {
      if (request.signal.aborted) abort();
      else request.signal.addEventListener('abort', abort, { once: true });
    }
  });
}

/**
 * The protocol handshake a client checks before it hands the executable a path: run once and
 * shared by every caller, and run again after it fails.
 */
export class Handshake {
  private pending: Promise<void> | undefined;

  constructor(private readonly check: (signal?: AbortSignal) => Promise<void>) {}

  async ensure(signal?: AbortSignal): Promise<void> {
    this.pending ??= this.check(signal);
    try {
      await this.pending;
    } catch (error: unknown) {
      this.pending = undefined;
      throw error;
    }
  }
}
