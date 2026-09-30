import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { createRequire } from 'node:module';
import path from 'node:path';
import { compileFunction } from 'node:vm';
import { test } from 'node:test';
import { transformSync } from 'esbuild';
import { DEFAULT_TEXTURE_PROFILE, type TextureConversionPlan } from '../../src/mods/textureConversion';
import { TEXTURE_SWIZZLES } from '../../src/mods/textureSwizzles';
import type { EddsConverter } from '../../src/platform/eddsConverter';
import type { renderTextureDraft } from '../../src/platform/textureConversion';
import type * as vscode from 'vscode';

test('the source pane is independent of every swizzle while the result reads the drafted EDDS and mip', async () => {
  const nodeRequire: (id: string) => unknown = createRequire(path.resolve('package.json'));
  const host = { Uri: { joinPath: (...parts: unknown[]) => ({ fsPath: parts.slice(1).join('/') }) },
    workspace: { fs: { createDirectory: () => Promise.resolve(), delete: () => Promise.resolve() } } };
  const module = { exports: {} as { renderTextureDraft: typeof renderTextureDraft } };
  const { code } = transformSync(readFileSync('src/platform/textureConversion.ts', 'utf8'), { loader: 'ts', format: 'cjs' });
  const execute = compileFunction(code, ['require', 'module', 'exports']) as (
    requireModule: (id: string) => unknown, module: unknown, exports: unknown,
  ) => void;
  execute((id: string) => {
    if (id === 'vscode') return host;
    if (id === '../mods/textureConversion') return { DEFAULT_TEXTURE_PROFILE };
    return id.startsWith('node:') ? nodeRequire(id) : {};
  }, module, module.exports);
  type ReadyPlan = Extract<TextureConversionPlan, { kind: 'ready' }>;
  for (const { name } of TEXTURE_SWIZZLES) {
    const conversions: ReadyPlan[] = [];
    const previews: [string, number][] = [];
    const converter = {
      convert: (plan: ReadyPlan) => {
        conversions.push(plan);
        return Promise.resolve({ width: 16, height: 8, pixelFormat: 'BGRA8' });
      },
      inspect: (output: string) => Promise.resolve({ output }),
      preview: (output: string, mip: number) => { previews.push([output, mip]); return Promise.resolve({ output, mip }); },
    } as unknown as EddsConverter;
    const plan: ReadyPlan = { kind: 'ready', scope: 'detached', action: 'convert', label: 'Convert',
      source: 'C:/mod/source.png', sourceFormat: 'PNG', output: 'C:/mod/source.edds', identityAction: 'none',
      profile: { ...DEFAULT_TEXTURE_PROFILE, Swizzling: name, RemoveMips: 1, Normalize: true, MipMapFilter: 'Kaiser' },
      revisions: { source: { size: 1, modified: 2 } } };
    const rendered = await module.exports.renderTextureDraft(plan, {} as vscode.Uri, converter, 2);
    assert.deepEqual(conversions[0]?.profile, { ...DEFAULT_TEXTURE_PROFILE, FormatCompress: 'Copy', GenerateMips: false });
    assert.strictEqual(conversions[1]?.profile, plan.profile);
    assert.deepEqual(previews, [['source.edds', 0], ['result.edds', 2]]);
    assert.deepEqual(rendered.source, { output: 'source.edds', mip: 0 });
    assert.deepEqual(rendered.result, { output: 'result.edds', mip: 2 });
    assert.deepEqual(rendered.sourceFacts, { width: 16, height: 8, hasAlpha: true });
  }
});
