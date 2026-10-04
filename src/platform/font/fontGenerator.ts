/** The bundled font CLI owns parsing, identity preservation and publication of all three files. */
import { randomUUID } from 'node:crypto';
import { unlink } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { machineFailureOf } from '../../mods/texture/edds';
import { bundledExecutable, type Execute, executeFile, Handshake } from '../nativeExecutable';
import type { FontCommandSession } from './fontCommands';

const OUTPUT_LIMIT = 1024 * 1024;

export interface FontSource {
  readonly family: string;
  readonly style: string;
}

export interface FontGeneration {
  readonly guid: string;
  readonly glyphCount: number;
  readonly missing: readonly number[];
}

export class FontGenerator {
  private readonly executable: string;
  private readonly handshake = new Handshake(async () => {
    const areas = recordOf((await this.invoke(['protocol', '--machine'])).areas);
    if (!Array.isArray(areas.font) || !areas.font.includes('inspect') || !areas.font.includes('generate')) {
      throw new Error('The bundled native executable does not support font generation. Reinstall the extension.');
    }
  });

  constructor(extensionPath: string, private readonly execute: Execute = executeFile) {
    this.executable = bundledExecutable(extensionPath);
  }

  async inspect(source: string): Promise<FontSource> {
    await this.handshake.ensure();
    const value = await this.invoke(['font', 'inspect', '--machine', '--protocol', '1', '--input', source]);
    if (value.kind !== 'font-source' || typeof value.family !== 'string' || typeof value.style !== 'string') {
      throw new Error('The font generator returned an invalid source description.');
    }
    return { family: value.family, style: value.style };
  }

  async generate(session: FontCommandSession, signal: AbortSignal): Promise<FontGeneration> {
    signal.throwIfAborted();
    await this.handshake.ensure();
    signal.throwIfAborted();
    const plan = session.plan;
    const args = ['font', 'generate', '--machine', '--protocol', '1', '--resource-name', plan.resourceName];
    for (const target of ['output', 'atlas', 'metadata'] as const) {
      const revision = session.revisions.get(plan[target]);
      args.push(`--expect-${target}-revision`, revision === undefined ? 'missing' : `${revision.size}:${revision.modified}`);
    }
    if (plan.action === 'regenerate') {
      args.push('--meta', plan.metadata);
    } else {
      args.push('--input', plan.source, '--output', plan.output, '--size', String(plan.size));
      if (plan.characters !== undefined) args.push('--characters', plan.characters);
    }
    const value = await this.invoke(args, signal);
    if (value.kind !== 'font-generate' || typeof value.guid !== 'string' || !/^[\da-f]{16}$/i.test(value.guid) ||
      typeof value.glyphCount !== 'number' || !Number.isInteger(value.glyphCount) || value.glyphCount < 1 ||
      !Array.isArray(value.missing) || !value.missing.every((code: unknown) =>
        typeof code === 'number' && Number.isInteger(code) && code >= 0 && code <= 0x10ffff)) {
      throw new Error('The font generator returned an invalid generation result.');
    }
    return { guid: value.guid, glyphCount: value.glyphCount, missing: value.missing as number[] };
  }

  /**
   * One command's JSON result. With a signal it gets a cancel file and is only ever asked to stop:
   * killing it during the three-file swap could defeat its rollback.
   */
  private async invoke(args: readonly string[], signal?: AbortSignal): Promise<Record<string, unknown>> {
    const cancelFile = signal === undefined ? undefined : path.join(tmpdir(), `enfusion-font-cancel-${randomUUID()}`);
    try {
      const result = await this.execute({
        executable: this.executable,
        args: [...args, ...(cancelFile === undefined ? [] : ['--cancel-file', cancelFile])],
        shell: false,
        windowsHide: true,
        maxBuffer: OUTPUT_LIMIT,
        signal,
        cancelFile,
      });
      const value = recordOf(JSON.parse(result.stdout));
      if (value.protocolVersion !== 1) throw new Error('The font generator returned an incompatible protocol.');
      return value;
    } catch (error: unknown) {
      throw failureOf(error);
    } finally {
      if (cancelFile !== undefined) await unlink(cancelFile).catch(() => undefined);
    }
  }
}

/** The CLI's own message when it reported one, an `AbortError` when it was cancelled. */
function failureOf(error: unknown): Error {
  const failed = recordOf(error);
  if (typeof failed.stdout !== 'string') return error instanceof Error ? error : new Error(String(error));
  try {
    const machine = machineFailureOf(failed.stdout);
    return Object.assign(new Error(machine.message), { name: machine.category === 'cancelled' ? 'AbortError' : 'Error' });
  } catch {
    // A missing executable or a crash has no JSON to read.
    const stderr = typeof failed.stderr === 'string' ? failed.stderr.trim() : '';
    const message = stderr || (error instanceof Error ? error.message : 'The font generator failed.');
    return Object.assign(new Error(message), { name: failed.name === 'AbortError' ? 'AbortError' : 'Error' });
  }
}

function recordOf(value: unknown): Record<string, unknown> {
  return typeof value === 'object' && value !== null && !Array.isArray(value) ? value as Record<string, unknown> : {};
}
