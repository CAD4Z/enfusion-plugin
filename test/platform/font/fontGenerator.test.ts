import assert from 'node:assert/strict';
import { test } from 'node:test';
import { fontCommandPlanOf, type FontCommandInput } from '../../../src/mods/font/fontCommands';
import { FontGenerator } from '../../../src/platform/font/fontGenerator';
import type { ExecutableRequest, ExecutableResult } from '../../../src/platform/nativeExecutable';

const PROTOCOL = JSON.stringify({
  protocolVersion: 1, kind: 'protocol', toolVersion: '0.2.0',
  areas: { edds: ['inspect', 'preview', 'convert', 'batch'], font: ['generate', 'inspect'] },
});

const INPUT: FontCommandInput = {
  platform: 'win32', scheme: 'file', roots: [{ root: 'C:/Mods/Example' }], kind: 'generate',
  file: 'C:/Mods/Example/Sans.ttf', name: 'SDF_Sans24', size: 24, characters: 'C:/Mods/Example/set.txt',
  existing: [], metadata: { kind: 'missing' },
};

function sessionOf(input: FontCommandInput) {
  const plan = fontCommandPlanOf(input);
  assert.equal(plan.kind, 'ready');
  if (plan.kind !== 'ready') throw new Error('unreachable');
  const revisions = new Map([
    [plan.output, undefined],
    [plan.atlas, { fingerprint: 'atlas', size: 10, modified: 20 }],
    [plan.metadata, undefined],
  ]);
  return { plan, input, revisions };
}

function answering(result: (request: ExecutableRequest) => Promise<ExecutableResult>, calls: ExecutableRequest[] = []) {
  return (request: ExecutableRequest): Promise<ExecutableResult> => {
    calls.push(request);
    return request.args[0] === 'protocol' ? Promise.resolve({ stdout: PROTOCOL, stderr: '' }) : result(request);
  };
}

test('font commands share one handshake and run the bundled executable without a shell', async () => {
  const calls: ExecutableRequest[] = [];
  const generator = new FontGenerator('C:\\extension', answering(() => Promise.resolve({
    stdout: JSON.stringify({ protocolVersion: 1, kind: 'font-source', family: 'Sans', style: 'Bold' }), stderr: '',
  }), calls));
  assert.deepEqual(await generator.inspect('C:\\Mods\\Example\\Sans.ttf'), { family: 'Sans', style: 'Bold' });
  await generator.inspect('C:\\Mods\\Example\\Sans.ttf');
  assert.deepEqual(calls.map((call) => call.args[0]), ['protocol', 'font', 'font']);
  assert.ok(calls.every((call) => call.shell === false && call.executable.endsWith('enfusion.exe')));
  assert.ok(calls.every((call) => call.cancelFile === undefined), 'only a cancellable generation watches a cancel file');
});

test('a generation names its source, size and confirmed revisions, and is only ever asked to stop', async () => {
  const calls: ExecutableRequest[] = [];
  const generator = new FontGenerator('C:\\extension', answering(() => Promise.resolve({
    stdout: JSON.stringify({ protocolVersion: 1, kind: 'font-generate', guid: '0123456789ABCDEF', glyphCount: 3, missing: [0x78] }),
    stderr: '',
  }), calls));
  const made = await generator.generate(sessionOf(INPUT), new AbortController().signal);
  assert.deepEqual(made, { guid: '0123456789ABCDEF', glyphCount: 3, missing: [0x78] });
  const call = calls.at(-1);
  assert.ok(call !== undefined);
  const value = (flag: string) => call.args[call.args.indexOf(flag) + 1];
  assert.equal(value('--expect-output-revision'), 'missing');
  assert.equal(value('--expect-atlas-revision'), '10:20');
  assert.equal(value('--input'), 'C:/Mods/Example/Sans.ttf');
  assert.equal(value('--size'), '24');
  assert.equal(value('--characters'), 'C:/Mods/Example/set.txt');
  assert.equal(value('--cancel-file'), call.cancelFile);
  assert.equal(call.killAfterMs, undefined, 'killing during the three-file swap could defeat its rollback');
});

test('a regeneration hands native code the recipe alone', async () => {
  const calls: ExecutableRequest[] = [];
  const generator = new FontGenerator('C:\\extension', answering(() => Promise.resolve({
    stdout: JSON.stringify({ protocolVersion: 1, kind: 'font-generate', guid: '0123456789ABCDEF', glyphCount: 3, missing: [] }),
    stderr: '',
  }), calls));
  await generator.generate(sessionOf({
    ...INPUT, kind: 'regenerate', file: 'C:/Mods/Example/SDF_Sans24.fnt', metadata: { kind: 'present' },
  }), new AbortController().signal);
  const args = calls.at(-1)?.args ?? [];
  assert.equal(args[args.indexOf('--meta') + 1], 'C:/Mods/Example/SDF_Sans24.fnt.meta');
  assert.equal(args.includes('--input') || args.includes('--size'), false);
});

test('a cancelled or refused generation reports the reason the CLI gave', async () => {
  const failing = (category: string, message: string) => new FontGenerator('C:\\extension', answering(() => Promise.reject(
    Object.assign(new Error('exit'), {
      stdout: JSON.stringify({ protocolVersion: 1, kind: 'error', error: { category, code: category, message } }),
      stderr: '',
    }),
  )));
  await assert.rejects(failing('cancelled', 'Font generation was cancelled.').generate(sessionOf(INPUT), new AbortController().signal),
    (error: unknown) => error instanceof Error && error.name === 'AbortError' && error.message.includes('cancelled'));
  await assert.rejects(failing('invalid-input', 'The font files changed.').generate(sessionOf(INPUT), new AbortController().signal),
    (error: unknown) => error instanceof Error && error.name === 'Error' && error.message === 'The font files changed.');
});
