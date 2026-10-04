/** The bundled font CLI owns parsing, identity preservation and publication of all three files. */
import { execFile } from 'node:child_process';
import { randomUUID } from 'node:crypto';
import { writeFileSync } from 'node:fs';
import { unlink } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import path from 'node:path';
import type { FontCommandSession } from './fontCommands';

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
  private handshake: Promise<void> | undefined;

  constructor(extensionPath: string) {
    this.executable = path.join(extensionPath, 'dist', 'native', 'win32-x64', 'enfusion.exe');
  }

  async inspect(source: string): Promise<FontSource> {
    await this.compatible();
    const value = await this.invoke(['font', 'inspect', '--machine', '--protocol', '1', '--input', source]);
    if (value.kind !== 'font-source' || typeof value.family !== 'string' || typeof value.style !== 'string') {
      throw new Error('The font generator returned an invalid source description.');
    }
    return { family: value.family, style: value.style };
  }

  async generate(session: FontCommandSession, signal: AbortSignal): Promise<FontGeneration> {
    signal.throwIfAborted();
    await this.compatible();
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
      if (plan.source === undefined || plan.size === undefined) throw new Error('The font generation plan is incomplete.');
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

  private async compatible(): Promise<void> {
    this.handshake ??= this.invoke(['protocol', '--machine']).then((value) => {
      const areas = recordOf(value.areas);
      if (value.kind !== 'protocol' || !Array.isArray(areas.font) ||
        !areas.font.includes('inspect') || !areas.font.includes('generate')) {
        throw new Error('The bundled native executable does not support font generation. Reinstall the extension.');
      }
    });
    try {
      await this.handshake;
    } catch (error: unknown) {
      this.handshake = undefined;
      throw error;
    }
  }

  private async invoke(args: readonly string[], signal?: AbortSignal): Promise<Record<string, unknown>> {
    const cancelFile = signal === undefined ? undefined : path.join(tmpdir(), `enfusion-font-cancel-${randomUUID()}`);
    try {
      return await new Promise<Record<string, unknown>>((resolve, reject) => {
        // Cancellation is cooperative: killing during the three-file swap could defeat rollback.
        const cancel = (): void => {
          try {
            if (cancelFile !== undefined) writeFileSync(cancelFile, '', { flag: 'w' });
          } catch {
            // A failed cancellation request must not terminate the extension host or interrupt publication.
          }
        };
        signal?.addEventListener('abort', cancel, { once: true });
        if (signal?.aborted === true) cancel();
        execFile(this.executable, [...args, ...(cancelFile === undefined ? [] : ['--cancel-file', cancelFile])],
          { encoding: 'utf8', shell: false, windowsHide: true, maxBuffer: 1024 * 1024 }, (error, stdout, stderr) => {
            signal?.removeEventListener('abort', cancel);
            try {
              if (error !== null) {
                let failure: Record<string, unknown> = {};
                try { failure = recordOf(recordOf(JSON.parse(stdout)).error); } catch { /* A missing executable has no JSON. */ }
                const message = typeof failure.message === 'string' ? failure.message : stderr.trim() || error.message;
                throw Object.assign(new Error(message), { name: failure.category === 'cancelled' ? 'AbortError' : 'Error' });
              }
              const value = recordOf(JSON.parse(stdout));
              if (value.protocolVersion !== 1) throw new Error('The font generator returned an incompatible protocol.');
              resolve(value);
            } catch (failure: unknown) {
              reject(failure instanceof Error ? failure : new Error(String(failure)));
            }
          });
      });
    } finally {
      if (cancelFile !== undefined) await unlink(cancelFile).catch(() => undefined);
    }
  }
}

function recordOf(value: unknown): Record<string, unknown> {
  return typeof value === 'object' && value !== null && !Array.isArray(value) ? value as Record<string, unknown> : {};
}
