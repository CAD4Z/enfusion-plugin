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
  type EddsConversion,
  type EddsInspection,
  type EddsPreview,
  conversionOf,
  inspectionOf,
  machineFailureOf,
  previewOf,
  protocolOf,
} from '../mods/edds';
import type { TextureConversionPlan } from '../mods/textureConversion';
import {
  type TextureConversion,
  textureConversionCapabilityOf,
  textureQualityText,
} from '../mods/textureConversions';
import type { TextureBatchEvent } from '../mods/textureBatchProtocol';
import type { TextureBatchJob } from '../mods/textureBatch';
import {
  type EddsBatchExecution,
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
      'edds-convert.exe',
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
            'inspect', '--machine', '--protocol', '1', '--input', input,
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
          ['preview', '--machine', '--protocol', '1', '--mip', String(mip), '--input', input],
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
      return conversionOf(
        await this.invoke(
          [
            'convert', '--machine', '--protocol', '1',
            '--input', plan.source,
            '--output', plan.output,
            '--target-format', 'enfusion-dds',
            '--format-compress', profile.FormatCompress.toLowerCase(),
            '--compress-threshold', String(profile.CompressTreshold),
            '--conversion', conversionWireOf(profile.Conversion),
            '--conversion-quality', textureQualityText(profile.ConversionQuality),
            '--swizzling', 'none',
            '--generate-mips', String(profile.GenerateMips),
            '--mipmap-function', 'filter',
            '--mipmap-filter', 'box',
            '--tiled-texture', 'true',
            '--expect-source-revision', revision(plan.revisions.source),
            '--expect-output-revision', revision(plan.revisions.output),
            '--expect-metadata-revision', revision(plan.revisions.metadata),
            ...registration,
          ],
          scheduledSignal,
        ),
      );
    }, signal);
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
