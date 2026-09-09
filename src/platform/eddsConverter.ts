/**
 * The one process boundary around the bundled EDDS converter.
 *
 * The executable path is installation-relative and is never configurable. Arguments go straight
 * to the process with no shell, so an EDDS path remains one opaque argument. Every useful command
 * waits for one cached protocol handshake before it hands a local path to the binary.
 */

import { execFile } from 'node:child_process';
import path from 'node:path';
import {
  type EddsFailureCategory,
  type EddsInspection,
  type EddsPreview,
  inspectionOf,
  machineFailureOf,
  previewOf,
  protocolOf,
} from '../mods/edds';

const OUTPUT_LIMIT = 96 * 1024 * 1024;

export interface ExecutableRequest {
  readonly executable: string;
  readonly args: readonly string[];
  readonly shell: false;
  readonly windowsHide: true;
  readonly maxBuffer: number;
  readonly signal?: AbortSignal;
}

export interface ExecutableResult {
  readonly stdout: string;
  readonly stderr: string;
}

export type Execute = (request: ExecutableRequest) => Promise<ExecutableResult>;

export class EddsConverterError extends Error {
  constructor(
    readonly category: EddsFailureCategory,
    readonly nativeCode: string,
    message: string,
    readonly diagnostic: string,
  ) {
    super(message);
    this.name = 'EddsConverterError';
  }
}

export class EddsConverter {
  private readonly executable: string;
  private handshake: Promise<void> | undefined;

  constructor(
    extensionPath: string,
    private readonly execute: Execute = executeFile,
  ) {
    this.executable = path.join(
      extensionPath,
      'dist',
      'native',
      'win32-x64',
      'edds-convert.exe',
    );
  }

  async inspect(input: string, signal?: AbortSignal): Promise<EddsInspection> {
    await this.compatible();
    return inspectionOf(
      await this.invoke(['inspect', '--machine', '--protocol', '1', '--input', input], signal),
    );
  }

  async preview(input: string, mip: number, signal?: AbortSignal): Promise<EddsPreview> {
    await this.compatible();
    return previewOf(
      await this.invoke(
        ['preview', '--machine', '--protocol', '1', '--mip', String(mip), '--input', input],
        signal,
      ),
    );
  }

  private async compatible(): Promise<void> {
    this.handshake ??= this.invoke(['protocol', '--machine']).then((source) => {
        protocolOf(source);
      });
    try {
      await this.handshake;
    } catch (error: unknown) {
      this.handshake = undefined;
      throw error;
    }
  }

  private async invoke(args: readonly string[], signal?: AbortSignal): Promise<string> {
    try {
      const result = await this.execute({
        executable: this.executable,
        args,
        shell: false,
        windowsHide: true,
        maxBuffer: OUTPUT_LIMIT,
        signal,
      });
      return result.stdout;
    } catch (error: unknown) {
      throw processFailure(error);
    }
  }
}

function executeFile(request: ExecutableRequest): Promise<ExecutableResult> {
  return new Promise<ExecutableResult>((resolve, reject) => {
    execFile(
      request.executable,
      [...request.args],
      {
        encoding: 'utf8',
        maxBuffer: request.maxBuffer,
        shell: request.shell,
        signal: request.signal,
        windowsHide: request.windowsHide,
      },
      (error, stdout, stderr) => {
        if (error === null) {
          resolve({ stdout, stderr });
          return;
        }
        reject(Object.assign(new Error(error.message), error, { stdout, stderr }));
      },
    );
  });
}

function processFailure(error: unknown): EddsConverterError {
  const failed = errorObject(error);
  const stdout = typeof failed.stdout === 'string' ? failed.stdout : '';
  const stderr = typeof failed.stderr === 'string' ? failed.stderr.trim() : '';
  const exit = typeof failed.code === 'number' ? failed.code : undefined;

  try {
    const machine = machineFailureOf(stdout);
    return new EddsConverterError(machine.category, machine.code, machine.message, stderr);
  } catch {
    const cancelled = failed.name === 'AbortError' || exit === 5;
    const category = cancelled ? 'cancelled' : categoryOf(exit);
    const message = stderr || (failed instanceof Error ? failed.message : 'The EDDS converter failed.');
    return new EddsConverterError(category, 'process-failed', message, stderr);
  }
}

function errorObject(error: unknown): Record<string, unknown> & { name?: string } {
  return typeof error === 'object' && error !== null
    ? (error as Record<string, unknown> & { name?: string })
    : {};
}

function categoryOf(exit: number | undefined): EddsFailureCategory {
  switch (exit) {
    case 2: return 'invalid-invocation';
    case 3: return 'invalid-input';
    case 4: return 'unsupported-format';
    case 5: return 'cancelled';
    case undefined: return 'internal-failure';
    default: return 'internal-failure';
  }
}
