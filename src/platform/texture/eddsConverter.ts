/**
 * The one process boundary around the bundled EDDS converter. Every useful command waits for one
 * cached protocol handshake before it hands a local path to the binary (see nativeExecutable).
 */

import { randomUUID } from 'node:crypto';
import { unlinkSync } from 'node:fs';
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
import { bundledExecutable, type Execute, executeFile, Handshake } from '../nativeExecutable';

export type { ExecutableRequest, ExecutableResult, Execute } from '../nativeExecutable';

const OUTPUT_LIMIT = 96 * 1024 * 1024;

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
  private readonly handshake = new Handshake(async (signal) => {
    protocolOf(await this.invoke(['protocol', '--machine'], signal));
  });

  constructor(
    extensionPath: string,
    private readonly execute: Execute = executeFile,
    private readonly scheduler = new TextureScheduler(),
  ) {
    this.executable = bundledExecutable(extensionPath);
  }

  async inspect(
    input: string,
    signal?: AbortSignal,
    metadata?: string,
    identityOnly = false,
  ): Promise<EddsInspection> {
    return this.scheduler.run('inspect', async (scheduledSignal) => {
      await this.handshake.ensure(scheduledSignal);
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
      await this.handshake.ensure(scheduledSignal);
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
      await this.handshake.ensure(scheduledSignal);
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
        // A conversion asked to stop rolls its pair back; one that will not is killed after this.
        killAfterMs: EDDS_BATCH_CANCEL_GRACE_MS,
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
      await this.handshake.ensure(scheduledSignal);
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
