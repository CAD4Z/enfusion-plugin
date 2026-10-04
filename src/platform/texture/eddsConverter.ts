/**
 * The one process boundary around the bundled EDDS converter.
 *
 * The executable path is installation-relative and is never configurable. Arguments go straight
 * to the process with no shell, so an EDDS path remains one opaque argument. Every useful command
 * waits for one cached protocol handshake before it hands a local path to the binary.
 */

import { type ChildProcess, execFile } from 'node:child_process';
import { randomUUID } from 'node:crypto';
import { unlinkSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { textureSwizzleWireOf } from '../../mods/texture/textureSwizzles';
import {
  EDDS_AREA,
  type EddsFailureCategory,
  type EddsConversion,
  type EddsInspection,
  type EddsPreview,
  conversionOf,
  inspectionOf,
  machineFailureOf,
  previewOf,
  protocolOf,
} from '../../mods/texture/edds';
import type { TextureConversionPlan } from '../../mods/texture/textureConversion';
import {
  type TextureConversion,
  textureConversionCapabilityOf,
  textureQualityText,
} from '../../mods/texture/textureConversions';
import type { TextureBatchEvent } from '../../mods/texture/textureBatchProtocol';
import type { TextureBatchJob } from '../../mods/texture/textureBatch';
import {
  EDDS_BATCH_CANCEL_GRACE_MS,
  type EddsBatchExecution,
  recoverTexturePair,
  runEddsBatch,
} from './eddsBatch';
import { TextureScheduler } from './textureScheduler';

const OUTPUT_LIMIT = 96 * 1024 * 1024;

export interface ExecutableRequest {
  readonly executable: string;
  readonly args: readonly string[];
  readonly shell: false;
  readonly windowsHide: true;
  readonly maxBuffer: number;
  readonly signal?: AbortSignal;
  /**
   * The file the process watches for a request to stop, when it has one. An abort then creates it
   * and kills the process only after a grace period, so a conversion can roll its pair back first.
   */
  readonly cancelFile?: string;
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
    private readonly scheduler = new TextureScheduler(),
  ) {
    this.executable = path.join(
      extensionPath,
      'dist',
      'native',
      'win32-x64',
      'enfusion.exe',
    );
  }

  async inspect(
    input: string,
    signal?: AbortSignal,
    metadata?: string,
    identityOnly = false,
  ): Promise<EddsInspection> {
    return this.scheduler.run('inspect', async (scheduledSignal) => {
      await this.compatible(scheduledSignal);
      return inspectionOf(
        await this.invoke(
          [
            EDDS_AREA, 'inspect', '--machine', '--protocol', '1', '--input', input,
            ...(metadata === undefined ? [] : ['--metadata', metadata]),
            ...(identityOnly ? ['--identity-only'] : []),
          ],
          scheduledSignal,
        ),
      );
    }, signal);
  }

  async preview(input: string, mip: number, signal?: AbortSignal): Promise<EddsPreview> {
    return this.scheduler.run('preview', async (scheduledSignal) => {
      await this.compatible(scheduledSignal);
      return previewOf(
        await this.invoke(
          [EDDS_AREA, 'preview', '--machine', '--protocol', '1', '--mip', String(mip), '--input', input, '--all-faces'],
          scheduledSignal,
        ),
      );
    }, signal);
  }

  async convert(
    plan: Extract<TextureConversionPlan, { kind: 'ready' }>,
    signal?: AbortSignal,
    workKind: 'convert' | 'preview' = 'convert',
  ): Promise<EddsConversion> {
    return this.scheduler.run(workKind, async (scheduledSignal) => {
      await this.compatible(scheduledSignal);
      const profile = plan.profile;
      const registration =
        plan.metadata === undefined || plan.identity === undefined
          ? []
          : [
              '--metadata', plan.metadata,
              '--resource-name', plan.identity.name,
              '--source-file', plan.identity.sourceFile,
              '--guid', plan.identity.guid,
            ];
      const revision = (value: { readonly size: number; readonly modified: number } | undefined) =>
        value === undefined ? 'missing' : `${value.size}:${value.modified}`;
      const cancelFile = path.join(tmpdir(), `enfusion-cancel-${randomUUID()}`);
      return conversionOf(
        await this.publish(
          plan,
          cancelFile,
          [
            EDDS_AREA, 'convert', '--machine', '--protocol', '1',
            '--input', plan.source,
            '--output', plan.output,
            '--target-format', 'enfusion-dds',
            '--format-compress', profile.FormatCompress.toLowerCase(),
            '--compress-threshold', String(profile.CompressTreshold),
            '--remove-mips', String(profile.RemoveMips),
            '--conversion', conversionWireOf(profile.Conversion),
            '--conversion-quality', textureQualityText(profile.ConversionQuality),
            '--swizzling', textureSwizzleWireOf(profile.Swizzling),
            '--contains-mips', String(profile.ContainsMips),
            '--generate-mips', String(profile.GenerateMips),
            '--generate-cubemap', String(profile.GenerateCubemap),
            '--normalize', String(profile.Normalize),
            '--mipmap-function', profile.MipMapFunction === 'ColorNoise'
              ? 'color-noise' : profile.MipMapFunction.toLowerCase(),
            '--mipmap-filter', profile.MipMapFilter.toLowerCase(),
            '--tiled-texture', String(profile.TiledTexture),
            '--expect-source-revision', revision(plan.revisions.source),
            '--expect-output-revision', revision(plan.revisions.output),
            '--expect-metadata-revision', revision(plan.revisions.metadata),
            ...registration,
            '--cancel-file', cancelFile,
          ],
          scheduledSignal,
        ),
      );
    }, signal);
  }

  /**
   * A conversion that did not finish may have died between moving the old pair aside and putting
   * the new one in place. Whatever the process left under its own id is put back the way a batch's
   * is — the old pair restored, or the committed new one kept — before the failure is reported.
   */
  private async publish(
    plan: Extract<TextureConversionPlan, { kind: 'ready' }>,
    cancelFile: string,
    args: readonly string[],
    signal?: AbortSignal,
  ): Promise<string> {
    try {
      const result = await this.execute({
        executable: this.executable,
        args,
        shell: false,
        windowsHide: true,
        maxBuffer: OUTPUT_LIMIT,
        signal,
        cancelFile,
      });
      return result.stdout;
    } catch (error: unknown) {
      const owner = errorObject(error).pid;
      if (typeof owner === 'number') {
        try {
          await recoverTexturePair(plan, String(owner));
        } catch (recoveryError: unknown) {
          const reason = recoveryError instanceof Error ? recoveryError.message : String(recoveryError);
          throw new EddsConverterError(
            'internal-failure',
            'recovery-failed',
            `The conversion stopped and its recovery could not finish; recovery files were retained. ${reason}`,
            reason,
          );
        }
      }
      throw processFailure(error);
    } finally {
      try { unlinkSync(cancelFile); } catch { /* It normally does not exist. */ }
    }
  }

  async batch(
    jobs: readonly TextureBatchJob[],
    onEvent: (event: TextureBatchEvent) => void,
    signal?: AbortSignal,
  ): Promise<EddsBatchExecution> {
    return this.scheduler.run('batch', async (scheduledSignal) => {
      await this.compatible(scheduledSignal);
      return runEddsBatch(
        this.executable,
        jobs,
        undefined,
        undefined,
        onEvent,
        scheduledSignal,
      );
    }, signal);
  }

  private async compatible(signal?: AbortSignal): Promise<void> {
    this.handshake ??= this.invoke(['protocol', '--machine'], signal).then((source) => {
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
      forced = setTimeout(() => child.kill('SIGKILL'), EDDS_BATCH_CANCEL_GRACE_MS);
    }
    if (graceful && request.signal !== undefined) {
      if (request.signal.aborted) abort();
      else request.signal.addEventListener('abort', abort, { once: true });
    }
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
    case undefined: return 'internal-failure';
    default: return 'internal-failure';
  }
}

/**
 * The wire name the CLI takes for a conversion. A conversion the contract does not carry cannot
 * reach here through a typed profile, and if one ever did, refusing it beats sending a flag the
 * converter would read as some other conversion.
 */
function conversionWireOf(conversion: TextureConversion): string {
  const capability = textureConversionCapabilityOf(conversion);
  if (!capability?.supported) {
    throw new Error(`Conversion ${conversion} is not supported by this converter.`);
  }
  return capability.wire;
}
