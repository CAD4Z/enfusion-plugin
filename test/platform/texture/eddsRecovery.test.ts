import assert from 'node:assert/strict';
import { execFile, execFileSync, spawn } from 'node:child_process';
import { mkdtemp, readFile, readdir, rm, stat, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { test, type TestContext } from 'node:test';
import type { TextureBatchJob } from '../../../src/mods/texture/textureBatch';
import { DEFAULT_TEXTURE_PROFILE } from '../../../src/mods/texture/textureConversion';
import { runEddsBatch, type SpawnBatchProcess } from '../../../src/platform/texture/eddsBatch';
import { EddsConverter, type Execute } from '../../../src/platform/texture/eddsConverter';

// CI runs this seam after building native code; the plain-Node job does not require a C compiler.
const executable = process.env.EDDS_TEST_CONVERTER;
const stages = [
  'output-backed-up', 'pair-backed-up', 'output-published', 'pair-published',
  'transaction-committed', 'output-cleaned',
] as const;
type Previous = 'pair' | 'output-only' | 'new';

for (const previous of ['pair', 'output-only', 'new'] as const) {
  for (const stage of stages) {
    test('native crash after ' + stage + ' recovers ' + previous, { skip: executable === undefined }, async (context) => {
      assert.ok(executable);
      const scene = await sceneOf(executable, previous, context);
      await assert.rejects(runEddsBatch(executable, [scene.job], crashingProcess(stage)), /code 6/);
      await scene.assertRecovered(stage);
    });

    test('a single conversion that dies after ' + stage + ' recovers ' + previous, { skip: executable === undefined }, async (context) => {
      assert.ok(executable);
      const scene = await sceneOf(executable, previous, context);
      const converter = new EddsConverter('unused', crashingExecute(executable, stage));
      await assert.rejects(converter.convert(scene.job.plan), { category: 'internal-failure' });
      await scene.assertRecovered(stage);
    });
  }
}

/** A source, and the pair a previous conversion left beside it, if any. */
async function sceneOf(converter: string, previous: Previous, context: TestContext) {
  const folder = await mkdtemp(path.join(tmpdir(), 'enfusion-native-recovery-'));
  context.after(async () => {
    assert.equal(path.dirname(path.resolve(folder)), path.resolve(tmpdir()));
    await rm(folder, { recursive: true, force: true });
  });
  const source = path.join(folder, 'source.tga');
  const output = path.join(folder, 'texture.edds');
  const metadata = output + '.meta';
  const image = Buffer.alloc(18 + 16);
  image[2] = 2;
  image.writeUInt16LE(2, 12);
  image.writeUInt16LE(2, 14);
  image[16] = 32;
  image[17] = 40;
  image.fill(255, 18);
  await writeFile(source, image);
  if (previous !== 'new') {
    execFileSync(converter, [
      'edds', 'convert', '--machine', '--protocol', '1', '--input', source, '--output', output,
      ...(previous === 'pair' ? [
        '--metadata', metadata, '--guid', '0123456789ABCDEF',
        '--resource-name', 'Mod/texture.edds', '--source-file', 'source.tga',
      ] : []),
    ], { windowsHide: true });
  }
  const before = {
    output: previous === 'new' ? undefined : await readFile(output),
    metadata: previous === 'pair' ? await readFile(metadata) : undefined,
  };
  // Rounded, the way VS Code's millisecond Date reads a file time.
  const revision = async (file: string) => {
    const value = await stat(file);
    return { size: value.size, modified: Math.round(value.mtimeMs) };
  };
  const job: TextureBatchJob = { id: 'one', plan: {
    kind: 'ready', scope: 'registered', action: 'convert', label: 'Convert',
    source, sourceFormat: 'TGA', output, metadata,
    identity: { guid: '0123456789ABCDEF', name: 'Mod/texture.edds', sourceFile: 'source.tga' },
    identityAction: previous === 'pair' ? 'preserve' : 'create',
    profile: { ...DEFAULT_TEXTURE_PROFILE, Conversion: 'Red', GenerateMips: false },
    revisions: {
      source: await revision(source),
      ...(previous === 'new' ? {} : { output: await revision(output) }),
      ...(previous === 'pair' ? { metadata: await revision(metadata) } : {}),
    },
  } };
  return {
    job,
    async assertRecovered(stage: (typeof stages)[number]) {
      const committed = stage === 'transaction-committed' || stage === 'output-cleaned';
      if (committed) {
        assert.notDeepEqual(await readFile(output), before.output);
        const result = await readFile(metadata, 'utf8');
        assert.match(result, /0123456789ABCDEF/);
        assert.match(result, /Conversion Red/);
      } else {
        for (const [file, expected] of [[output, before.output], [metadata, before.metadata]] as const) {
          if (expected === undefined) await assert.rejects(stat(file), { code: 'ENOENT' });
          else assert.deepEqual(await readFile(file), expected);
        }
      }
      assert.deepEqual((await readdir(folder)).filter((file) => file.includes('.enfusion-')), []);
    },
  };
}

function crashingProcess(stage: string): SpawnBatchProcess {
  return (request) => {
    const child = spawn(request.executable, [...request.args], {
      shell: false, windowsHide: true, stdio: ['pipe', 'pipe', 'pipe'],
      env: { ...process.env, EDDS_CONVERT_FAIL: 'crash-' + stage },
    });
    child.stdout.setEncoding('utf8');
    child.stderr.setEncoding('utf8');
    return {
      tempOwner: String(child.pid),
      completed: new Promise((resolve, reject) => {
        child.once('error', reject);
        child.once('close', (code, signal) => resolve({ code, signal }));
      }),
      write(value) { child.stdin.write(value); },
      end() { child.stdin.end(); },
      onStdout(listener) { child.stdout.on('data', listener); },
      onStderr(listener) { child.stderr.on('data', listener); },
      interrupt() { child.kill(); },
      kill() { child.kill(); },
    };
  };
}

/** The production executor's contract, with the crash asked for only on the conversion itself. */
function crashingExecute(converter: string, stage: string): Execute {
  return (request) => new Promise((resolve, reject) => {
    const crash = request.args[1] === 'convert' ? { EDDS_CONVERT_FAIL: 'crash-' + stage } : {};
    const child = execFile(converter, [...request.args], {
      encoding: 'utf8', windowsHide: true, env: { ...process.env, ...crash },
    }, (error, stdout, stderr) => {
      if (error === null) resolve({ stdout, stderr });
      else reject(Object.assign(new Error(error.message), error, { stdout, stderr, pid: child.pid }));
    });
  });
}
