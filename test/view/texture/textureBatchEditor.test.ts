import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { createRequire } from 'node:module';
import path from 'node:path';
import { setImmediate } from 'node:timers/promises';
import { compileFunction } from 'node:vm';
import { test } from 'node:test';
import { transformSync } from 'esbuild';
import type * as vscode from 'vscode';
import type { EddsConverter } from '../../../src/platform/texture/eddsConverter';
import type { TextureBatchEditor } from '../../../src/view/texture/textureBatchEditor';
import type { TextureBatchRequest } from '../../../src/webview/texture/textureBatchProtocol';
import { textureBatchPlanOf } from '../../../src/mods/texture/textureBatch';
import type { TextureBatchEvent } from '../../../src/mods/texture/textureBatchProtocol';

// Exercise the real editor callbacks and reducer; only VS Code and the I/O adapters are replaced.
// Loading on Node avoids opening an Extension Host just to hold an asynchronous refresh in flight.
function editorHarness() {
  const uri = {
    fsPath: 'C:/mod/Mod/a.png',
    toString: () => 'file:///C:/mod/Mod/a.png',
  } as vscode.Uri;
  const plan = textureBatchPlanOf({
    primary: uri.fsPath,
    roots: [{ root: 'C:/mod', prefixRoot: 'C:/mod/Mod' }],
    occupiedGuids: [],
    items: [{
      source: uri.fsPath, kind: 'file',
      sourceRevision: { size: 1, modified: 2 },
      metadata: { kind: 'missing' }, newGuid: '0123456789ABCDEF',
    }],
  });
  let message: (request: TextureBatchRequest) => void = () => { throw new Error('No editor'); };
  let dispose: () => void = () => { throw new Error('No editor'); };
  let finishLoad: (value: typeof plan) => void = () => { throw new Error('No pending load'); };
  let holdLoad = false;
  let previews = 0;
  const calls: AbortSignal[] = [];
  const panel = {
    webview: {
      asWebviewUri: (value: unknown) => value,
      postMessage: () => Promise.resolve(true),
      onDidReceiveMessage: (listener: typeof message) => {
        message = listener;
        return { dispose() { /* The harness owns the callback. */ } };
      },
    },
    onDidDispose: (listener: () => void) => { dispose = listener; },
  };
  const host = {
    window: { createWebviewPanel: () => panel },
    ViewColumn: { Active: 1 },
    Uri: { joinPath: () => ({ toString: () => 'asset' }) },
  };
  const adapter = {
    loadTextureBatch: () => holdLoad
      ? new Promise<typeof plan>((resolve) => { finishLoad = resolve; })
      : Promise.resolve(plan),
    renderTextureDraft: () => {
      previews += 1;
      return Promise.reject(new Error('No preview in this fixture'));
    },
  };
  const converter = {
    batch: (_jobs: unknown, emit: (value: TextureBatchEvent) => void, signal: AbortSignal) => {
      calls.push(signal);
      emit({ protocolVersion: 1, kind: 'result', id: '0', status: 'Failed', reason: 'locked', retryable: true });
      emit({ protocolVersion: 1, kind: 'complete', converted: 0, failed: 1, cancelled: 0 });
      return Promise.resolve();
    },
  } as unknown as EddsConverter;
  const nodeRequire: (id: string) => unknown = createRequire(path.resolve('package.json'));
  const cache = new Map<string, Record<string, unknown>>();
  const load = (filename: string): Record<string, unknown> => {
    const cached = cache.get(filename);
    if (cached !== undefined) return cached;
    const module = { exports: {} as Record<string, unknown> };
    cache.set(filename, module.exports);
    const { code } = transformSync(readFileSync(filename, 'utf8'), { loader: 'ts', format: 'cjs' });
    const requireModule = (id: string): unknown => {
      if (id === 'vscode') return host;
      if (id.endsWith('/platform/texture/textureConversion')) return adapter;
      if (id.endsWith('/platform/texture/eddsBatch')) return { batchFailureRetryable: () => true };
      return id.startsWith('.') ? load(path.resolve(path.dirname(filename), id + '.ts')) : nodeRequire(id);
    };
    const execute = compileFunction(code, ['require', 'module', 'exports']) as (
      requireModule: (id: string) => unknown,
      module: { exports: Record<string, unknown> },
      exports: Record<string, unknown>,
    ) => void;
    execute(requireModule, module, module.exports);
    cache.set(filename, module.exports);
    return module.exports;
  };
  const Editor = load(path.resolve('src/view/texture/textureBatchEditor.ts')).TextureBatchEditor as typeof TextureBatchEditor;
  new Editor(uri, uri, converter, {
    error() { /* Expected failures stay in the fixture. */ },
  } as unknown as vscode.LogOutputChannel).open(uri, []);
  return {
    calls,
    previews: () => previews,
    send: (request: TextureBatchRequest) => message(request),
    close: () => dispose(),
    hold: () => { holdLoad = true; },
    finish: () => finishLoad(plan),
  };
}

test('closing while Retry Failed refreshes never starts another conversion', async () => {
  const editor = editorHarness();
  editor.send({ type: 'ready' });
  await setImmediate();
  editor.send({ type: 'run' });
  await setImmediate();
  assert.equal(editor.calls.length, 1);
  editor.hold();
  editor.send({ type: 'retry-failed' });
  editor.close();
  editor.finish();
  await setImmediate();
  assert.equal(editor.calls.length, 1);
});

test('closing during the initial load never starts a preview afterwards', async () => {
  const editor = editorHarness();
  editor.hold();
  editor.send({ type: 'ready' });
  editor.close();
  editor.finish();
  await setImmediate();
  assert.equal(editor.previews(), 0);
  assert.equal(editor.calls.length, 0);
});
