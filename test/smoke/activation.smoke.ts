import { execFile, spawn } from 'node:child_process';
import assert from 'node:assert/strict';
import path from 'node:path';
import { promisify } from 'node:util';
import { deflateSync } from 'node:zlib';
import * as vscode from 'vscode';
import { inspectionOf, previewOf } from '../../src/mods/texture/edds';
import { analysisOf, pixelAt } from '../../src/mods/texture/textureAnalysis';
import { DEFAULT_TEXTURE_PROFILE } from '../../src/mods/texture/textureConversion';
import { assertFontCommandCurrent, loadFontCommand } from '../../src/platform/font/fontCommands';
import { FontGenerator } from '../../src/platform/font/fontGenerator';

const executeFile = promisify(execFile);

/** Runs inside VS Code against the directory installed from the packaged VSIX. */
export async function run(): Promise<void> {
  const extension = vscode.extensions.getExtension('hurfy.enfusion-plugin');
  assert.ok(extension, 'the clean profile did not discover hurfy.enfusion-plugin');
  assert.equal(
    path.resolve(extension.extensionPath),
    path.resolve(requiredEnvironment('EXPECTED_EXTENSION_PATH')),
    'the smoke did not activate the installed VSIX directory',
  );

  await extension.activate();
  const commands = await vscode.commands.getCommands(true);
  assert.ok(commands.includes('enfusion.edds.openPreview'), 'the EDDS preview command is not registered');
  assert.ok(commands.includes('enfusion.texture.convert'), 'the texture conversion command is not registered');
  assert.ok(commands.includes('enfusion.font.generate'), 'the font generation command is not registered');
  assert.ok(commands.includes('enfusion.font.regenerate'), 'the font regeneration command is not registered');

  const workspace = vscode.workspace.workspaceFolders?.[0];
  assert.ok(workspace, 'the smoke has no local workspace for its owned fixture');
  const fixture = vscode.Uri.joinPath(workspace.uri, 'activation-smoke.edds');
  await vscode.workspace.fs.writeFile(fixture, copyFixture());
  await vscode.commands.executeCommand('enfusion.edds.openPreview', fixture);
  await eventually(
    () =>
      vscode.window.tabGroups.all.some((group) =>
        group.tabs.some(
          (tab) =>
            tab.input instanceof vscode.TabInputCustom &&
            tab.input.viewType === 'enfusion.eddsPreview' &&
            tab.input.uri.toString() === fixture.toString(),
        ),
      ),
    'the EDDS custom editor did not open',
  );

  await vscode.workspace.fs.writeFile(
    vscode.Uri.joinPath(workspace.uri, 'workspace.enf'),
    new TextEncoder().encode('{}\n'),
  );
  const source = vscode.Uri.joinPath(workspace.uri, 'activation-smoke.png');
  await vscode.workspace.fs.writeFile(source, pngFixture());
  await vscode.commands.executeCommand('enfusion.texture.convert', source);
  await eventually(
    () =>
      vscode.window.tabGroups.all.some((group) =>
        group.tabs.some(
          (tab) =>
            tab.input instanceof vscode.TabInputCustom &&
            tab.input.viewType === 'enfusion.textureConversion' &&
            tab.input.uri.toString() === source.toString(),
        ),
      ),
    'the PNG Explorer conversion editor did not open',
  );

  const executable = path.join(
    extension.extensionPath,
    'dist',
    'native',
    'win32-x64',
    'enfusion.exe',
  );
  const result = await executeFile(executable, ['protocol', '--machine'], {
    encoding: 'utf8',
    shell: false,
    windowsHide: true,
  });
  const protocol: unknown = JSON.parse(result.stdout);
  assert.deepEqual(protocol, {
    protocolVersion: 1,
    kind: 'protocol',
    toolVersion: '0.2.0',
    areas: { edds: ['inspect', 'preview', 'convert', 'batch'], font: ['generate', 'inspect'] },
  });
  await fontSmoke(extension.extensionPath, workspace.uri, executable);

  const secondSource = vscode.Uri.joinPath(workspace.uri, 'activation-smoke.tga');
  await vscode.workspace.fs.writeFile(secondSource, tgaFixture());
  await vscode.commands.executeCommand(
    'enfusion.texture.convert',
    source,
    [secondSource, source],
  );
  await eventually(
    () => vscode.window.tabGroups.all.some((group) => group.tabs.some(
      (tab) => tab.input instanceof vscode.TabInputWebview &&
        tab.input.viewType.endsWith('enfusion.textureBatchConversion'),
    )),
    'Explorer multi-select did not open one texture batch editor',
  );

  const jpgSource = vscode.Uri.joinPath(workspace.uri, 'activation-smoke.jpg');
  const tiffSource = vscode.Uri.joinPath(workspace.uri, 'activation-smoke.tiff');
  await vscode.workspace.fs.writeFile(jpgSource, jpgFixture());
  await vscode.workspace.fs.writeFile(tiffSource, tiffFixture());

  const batchPng = vscode.Uri.joinPath(workspace.uri, 'packaged-batch-png.edds');
  const batchTga = vscode.Uri.joinPath(workspace.uri, 'packaged-batch-tga.edds');
  const batchJpg = vscode.Uri.joinPath(workspace.uri, 'packaged-batch-jpg.edds');
  const batchTiff = vscode.Uri.joinPath(workspace.uri, 'packaged-batch-tiff.edds');
  const batch = await executeBatchProcess(executable, batchInput([
    { id: 'png', input: source.fsPath, output: batchPng.fsPath },
    { id: 'tga', input: secondSource.fsPath, output: batchTga.fsPath },
    { id: 'jpg', input: jpgSource.fsPath, output: batchJpg.fsPath },
    { id: 'tiff', input: tiffSource.fsPath, output: batchTiff.fsPath },
  ]));
  // A pool reports a row when its own image finishes, so the set is the fact here, not the order.
  assert.deepEqual(
    batch
      .filter(({ kind }) => kind === 'result')
      .map(({ id, status }) => [id, status])
      .sort((left, right) => String(left[0]).localeCompare(String(right[0]))),
    [
      ['jpg', 'Converted'],
      ['png', 'Converted'],
      ['tga', 'Converted'],
      ['tiff', 'Converted'],
    ],
  );
  assert.deepEqual(batch.at(-1), {
    protocolVersion: 1, kind: 'complete', converted: 4, failed: 0, cancelled: 0,
  });
  for (const output of [batchPng, batchTga, batchJpg, batchTiff]) {
    assert.equal((await vscode.workspace.fs.stat(output)).type, vscode.FileType.File);
  }

  const converted = vscode.Uri.joinPath(workspace.uri, 'packaged-conversion.edds');
  const conversion = await executeFile(
    executable,
    [
      'edds', 'convert', '--machine', '--protocol', '1',
      '--input', source.fsPath, '--output', converted.fsPath,
      '--target-format', 'enfusion-dds', '--format-compress', 'fastest',
      '--compress-threshold', '80', '--remove-mips', '0',
      '--conversion', 'none', '--conversion-quality', '1', '--swizzling', 'none',
      '--contains-mips', 'false', '--generate-mips', 'true', '--normalize', 'false',
      '--mipmap-function', 'filter',
      '--mipmap-filter', 'box', '--tiled-texture', 'true',
    ],
    { encoding: 'utf8', shell: false, windowsHide: true },
  );
  assert.deepEqual(JSON.parse(conversion.stdout), {
    protocolVersion: 1,
    kind: 'convert',
    width: 2,
    height: 1,
    mipCount: 2,
    pixelFormat: 'BGRA8',
    registered: false,
  });
  assert.equal((await vscode.workspace.fs.stat(converted)).type, vscode.FileType.File);

  // The installed executable's actual pixels feed the same read-only analysis as the webviews.
  const analysisInspection = inspectionOf((await executeFile(executable,
    ['edds', 'inspect', '--machine', '--protocol', '1', '--input', converted.fsPath],
    { encoding: 'utf8', shell: false, windowsHide: true })).stdout);
  const analysisResult = previewOf((await executeFile(executable,
    ['edds', 'preview', '--machine', '--protocol', '1', '--mip', '0', '--input', converted.fsPath],
    { encoding: 'utf8', shell: false, windowsHide: true })).stdout);
  const analysis = analysisOf({ inspection: analysisInspection, result: analysisResult,
    source: { level: 0, width: 2, height: 1, rgba: Uint8Array.from([10, 20, 30, 40, 50, 60, 70, 80]) },
    profile: DEFAULT_TEXTURE_PROFILE }, { side: 'result', channel: 'alpha' });
  assert.equal(analysis.histogram.kind === 'available' && analysis.histogram.value.series[0]?.bins[40], 1);
  assert.equal(analysis.error.kind === 'available' && analysis.error.value.rmse, 0);
  assert.deepEqual(analysis.memory, { kind: 'available', value: { mipBytes: 8, chainBytes: 12 } });
  const sample = pixelAt({ ...analysisResult, channels: 'RGBA' }, { x: 3, y: 1 },
    { left: 0, top: 0, width: 4, height: 2 });
  assert.equal(sample.kind === 'available' && sample.value.values[3], 80);
  for (const bundle of ['texture.js', 'texture-conversion.js']) {
    const script = new TextDecoder().decode(await vscode.workspace.fs.readFile(
      vscode.Uri.joinPath(extension.extensionUri, 'dist', bundle)));
    assert.ok(script.includes('Pixel inspector') && script.includes('Result runtime memory'), `${bundle} lacks analysis controls`);
  }

  // Every runtime format, out of the executable that actually ships, not the one CI just built.
  for (const [conversion, runtime, channels] of [
    ['dxt-compression', 'DXT5', 'RGBA'],
    ['red', 'R8', 'R'],
    ['red-hq-compression', 'BC4', 'R'],
    ['red-green', 'RG8', 'RG'],
    ['red-green-hq-compression', 'BC5', 'RG'],
    ['color-hq-compression', 'BC7', 'RGBA'],
  ] as const) {
    const output = vscode.Uri.joinPath(workspace.uri, `packaged-${conversion}.edds`);
    const result = await executeFile(
      executable,
      [
        'edds', 'convert', '--machine', '--protocol', '1',
        '--input', source.fsPath, '--output', output.fsPath,
        '--target-format', 'enfusion-dds', '--format-compress', 'fastest',
        '--compress-threshold', '80', '--conversion', conversion,
        '--conversion-quality', '1',
        '--swizzling', 'none', '--generate-mips', 'true', '--mipmap-function', 'filter',
        '--mipmap-filter', 'box', '--tiled-texture', 'true',
      ],
      { encoding: 'utf8', shell: false, windowsHide: true },
    );
    assert.equal(machineValue(result.stdout).pixelFormat, runtime);

    const inspected = await executeFile(
      executable,
      ['edds', 'inspect', '--machine', '--protocol', '1', '--input', output.fsPath],
      { encoding: 'utf8', shell: false, windowsHide: true },
    );
    const facts = machineValue(inspected.stdout);
    assert.equal(facts.pixelFormat, runtime);
    assert.equal(facts.channels, channels);
    assert.equal(facts.previewSupported, true);

    const previewed = await executeFile(
      executable,
      ['edds', 'preview', '--machine', '--protocol', '1', '--mip', '0', '--input', output.fsPath],
      { encoding: 'utf8', shell: false, windowsHide: true },
    );
    assert.equal(machineValue(previewed.stdout).byteLength, 2 * 1 * 4);
  }

  // HDRCompression needs a Radiance source: the shipped executable refuses it for this PNG too.
  await assert.rejects(
    executeFile(
      executable,
      [
        'edds', 'convert', '--machine', '--protocol', '1',
        '--input', source.fsPath,
        '--output', vscode.Uri.joinPath(workspace.uri, 'packaged-hdr.edds').fsPath,
        '--conversion', 'hdr-compression',
      ],
      { encoding: 'utf8', shell: false, windowsHide: true },
    ),
  );
}

async function fontSmoke(extensionPath: string, workspace: vscode.Uri, executable: string): Promise<void> {
  const generator = new FontGenerator(extensionPath);
  // A mod of its own, so that a font inside its prefix root gets a recipe; the font beside the
  // workspace file below gets none, as a texture there gets no registration.
  const mod = vscode.Uri.joinPath(workspace, 'SmokeMod');
  const fonts = vscode.Uri.joinPath(mod, 'SmokeMod', 'Fonts');
  await vscode.workspace.fs.writeFile(vscode.Uri.joinPath(mod, 'mod.enf'), new TextEncoder().encode('{ "name": "SmokeMod" }\n'));
  await vscode.workspace.fs.writeFile(vscode.Uri.joinPath(mod, 'SmokeMod', 'config.cpp'), new TextEncoder().encode(
    'class CfgPatches { class SmokeMod { requiredAddons[] = {}; }; };\nclass CfgMods { class SmokeMod { dir = "SmokeMod"; }; };\n'));
  for (const file of ['Font smoke.ttf', 'Font smoke.txt']) {
    await vscode.workspace.fs.copy(vscode.Uri.joinPath(workspace, file), vscode.Uri.joinPath(fonts, file));
  }
  const source = vscode.Uri.joinPath(fonts, 'Font smoke.ttf');
  const characters = vscode.Uri.joinPath(fonts, 'Font smoke.txt');
  // "SDF_Proverka32", Russian for "check": a name outside ASCII must survive the CLI and the recipe.
  const name = 'SDF_\u041f\u0440\u043e\u0432\u0435\u0440\u043a\u043032';
  assert.deepEqual(await generator.inspect(source.fsPath), { family: 'Fixture Sans', style: 'Regular' });
  const session = await loadFontCommand(source, {
    kind: 'generate', name, size: 32, characters: characters.fsPath,
  });
  await assertFontCommandCurrent(session);
  const made = await generator.generate(session, new AbortController().signal);
  assert.deepEqual(made.missing, [0x78, 0x416]);
  const output = vscode.Uri.file(session.plan.output);
  const metadata = vscode.Uri.file(session.plan.metadata);
  const atlas = vscode.Uri.file(session.plan.atlas);
  const recipe = new TextDecoder().decode(await vscode.workspace.fs.readFile(metadata));
  assert.match(recipe, /SourceFile "Font smoke.ttf"/);
  assert.match(recipe, /Characters "Font smoke.txt"/);
  assert.ok(made.guid !== undefined && recipe.includes(`{${made.guid}}SmokeMod/Fonts/${name}.fnt`));
  const inspected = machineValue((await executeFile(executable,
    ['font', 'inspect', '--machine', '--protocol', '1', '--input', output.fsPath],
    { encoding: 'utf8', shell: false, windowsHide: true })).stdout);
  assert.equal(inspected.size, 32);
  assert.equal(inspected.type, 2);
  const texture = inspectionOf((await executeFile(executable,
    ['edds', 'inspect', '--machine', '--protocol', '1', '--input', atlas.fsPath],
    { encoding: 'utf8', shell: false, windowsHide: true })).stdout);
  assert.equal(texture.pixelFormat, 'BGRA8');
  assert.equal(texture.mips.length, 1);

  // A real installed command, with no prompts: the noncanonical comment disappears on regeneration.
  await vscode.workspace.fs.writeFile(metadata, new TextEncoder().encode(recipe + '\n// regenerate smoke\n'));
  await vscode.commands.executeCommand('enfusion.font.regenerate', output);
  assert.equal(new TextDecoder().decode(await vscode.workspace.fs.readFile(metadata)), recipe);
  const replacement = await loadFontCommand(source, { kind: 'generate', name, size: 24 });
  assert.equal(replacement.plan.action, 'replace');
  const replaced = await generator.generate(replacement, new AbortController().signal);
  assert.equal(replaced.guid, made.guid);
  // The ring of A with ring is 0.025 em, 0.6 atlas pixels at size 24: the one stroke the atlas cannot hold.
  assert.deepEqual(replaced.thin, [0xc5]);
  // Native publication must reject the old confirmation even if the caller skips its early check.
  await assert.rejects(generator.generate(session, new AbortController().signal), /changed/);

  // Unreadable native recipes refuse without changing either runtime artifact.
  const before = await Promise.all([output, atlas].map((uri) => vscode.workspace.fs.readFile(uri)));
  await vscode.workspace.fs.writeFile(metadata, new TextEncoder().encode('not a recipe'));
  const broken = await loadFontCommand(output, { kind: 'regenerate' });
  await assert.rejects(generator.generate(broken, new AbortController().signal), /recipe|metadata/i);
  assert.deepEqual(await Promise.all([output, atlas].map((uri) => vscode.workspace.fs.readFile(uri))), before);
  await assert.rejects(assertFontCommandCurrent(replacement), /changed/);
  await vscode.workspace.fs.delete(metadata);
  await assert.rejects(loadFontCommand(output, { kind: 'regenerate' }), /no .fnt.meta recipe/);

  // Outside every prefix root: the font and its atlas alone, and no recipe to regenerate from.
  const loose = await loadFontCommand(vscode.Uri.joinPath(workspace, 'Font smoke.ttf'), {
    kind: 'generate', name, size: 32, characters: vscode.Uri.joinPath(workspace, 'Font smoke.txt').fsPath,
  });
  assert.equal(loose.plan.resourceName, undefined);
  assert.equal((await generator.generate(loose, new AbortController().signal)).guid, undefined);
  assert.equal((await vscode.workspace.fs.stat(vscode.Uri.file(loose.plan.atlas))).type, vscode.FileType.File);
  await assert.rejects(Promise.resolve(vscode.workspace.fs.stat(vscode.Uri.file(loose.plan.metadata))));
  await assert.rejects(loadFontCommand(vscode.Uri.file(loose.plan.output), { kind: 'regenerate' }), /no recipe to regenerate from/);
}

/** One machine-protocol result as a plain record, so a smoke assertion never indexes `any`. */
function machineValue(stdout: string): Record<string, unknown> {
  return JSON.parse(stdout) as Record<string, unknown>;
}

interface SmokeBatchJob {
  readonly id: string;
  readonly input: string;
  readonly output: string;
}

function batchInput(jobs: readonly SmokeBatchJob[]): string {
  const profile = {
    TargetFormat: 'EnfusionDDS', FormatCompress: 'Fastest', CompressTreshold: 80,
    RemoveMips: 0, Conversion: 'None', ConversionQuality: 1, Swizzling: 'None',
    ContainsMips: false, GenerateMips: false, Normalize: false,
    MipMapFunction: 'Filter', MipMapFilter: 'Box', TiledTexture: true,
  };
  return [
    { protocolVersion: 1, kind: 'batch', jobCount: jobs.length },
    ...jobs.map((job) => ({
      protocolVersion: 1,
      kind: 'job',
      ...job,
      metadata: null,
      identity: null,
      profile,
      expected: null,
    })),
    { protocolVersion: 1, kind: 'end' },
  ].map((record) => JSON.stringify(record)).join('\n') + '\n';
}

function executeBatchProcess(executable: string, input: string): Promise<Record<string, unknown>[]> {
  return new Promise((resolve, reject) => {
    const child = spawn(executable, ['edds', 'batch', '--machine', '--protocol', '1'], {
      shell: false,
      windowsHide: true,
      stdio: ['pipe', 'pipe', 'pipe'],
    });
    let stdout = '';
    let stderr = '';
    child.stdout.setEncoding('utf8');
    child.stderr.setEncoding('utf8');
    child.stdout.on('data', (chunk: string) => { stdout += chunk; });
    child.stderr.on('data', (chunk: string) => { stderr += chunk; });
    child.once('error', reject);
    child.once('close', (code) => {
      if (code !== 0) {
        reject(new Error(stderr || `installed batch process exited with ${String(code)}`));
        return;
      }
      resolve(stdout.trim().split(/\r?\n/).map((line) => JSON.parse(line) as Record<string, unknown>));
    });
    child.stdin.end(input, 'utf8');
  });
}

function copyFixture(): Uint8Array {
  const bytes = new Uint8Array(140);
  const words = new DataView(bytes.buffer);
  putText(bytes, 0, 'DDS ');
  words.setUint32(4, 124, true);
  words.setUint32(8, 0x0002100f, true);
  words.setUint32(12, 1, true);
  words.setUint32(16, 1, true);
  words.setUint32(20, 4, true);
  words.setUint32(28, 1, true);
  putText(bytes, 36, 'ENF1');
  words.setUint32(76, 32, true);
  words.setUint32(80, 0x41, true);
  words.setUint32(88, 32, true);
  words.setUint32(92, 0x00ff0000, true);
  words.setUint32(96, 0x0000ff00, true);
  words.setUint32(100, 0x000000ff, true);
  words.setUint32(104, 0xff000000, true);
  words.setUint32(108, 0x1000, true);
  putText(bytes, 128, 'COPY');
  words.setUint32(132, 4, true);
  bytes.set([3, 2, 1, 255], 136);
  return bytes;
}

function pngFixture(): Uint8Array {
  const signature = Uint8Array.from([137, 80, 78, 71, 13, 10, 26, 10]);
  const ihdr = new Uint8Array(13);
  const view = new DataView(ihdr.buffer);
  view.setUint32(0, 2);
  view.setUint32(4, 1);
  ihdr.set([8, 6, 0, 0, 0], 8);
  const pixels = Uint8Array.from([0, 10, 20, 30, 40, 50, 60, 70, 80]);
  return join(signature, pngChunk('IHDR', ihdr), pngChunk('IDAT', deflateSync(pixels)), pngChunk('IEND', new Uint8Array()));
}

function tgaFixture(): Uint8Array {
  return Uint8Array.from([
    0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    1, 0, 1, 0, 24, 0x20,
    30, 20, 10,
  ]);
}

/**
 * The same two sources the native suite owns, carried here as bytes: what this smoke proves is
 * that the packaged executable has their codecs, not that the fixtures can be built again in
 * TypeScript. 16x8 baseline YCbCr, and 3x2 uncompressed RGB.
 */
function jpgFixture(): Uint8Array {
  return Buffer.from(
    '/9j/2wBDAAEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEB' +
      'AQEBAQEBAQEBAQH/wAARCAAIABADAREAAhEAAxEA/8QAFgABAgAAAAAAAAAAAAAAAAAAAAkK/8QAFBAB' +
      'AAAAAAAAAAAAAAAAAAAAAP/aAAwDAQACAAMAAD8AjeDyAH//2Q==',
    'base64',
  );
}

function tiffFixture(): Uint8Array {
  return Buffer.from(
    'SUkqAAgAAAAJAAABAwABAAAAAwAAAAEBAwABAAAAAgAAAAIBAwADAAAAegAAAAMBAwABAAAAAQAAAAYB' +
      'AwABAAAAAgAAABEBBAABAAAAgAAAABUBAwABAAAAAwAAABYBAwABAAAAAgAAABcBBAABAAAAEgAAAAAA' +
      'AAAIAAgACAAKFB4oMjxGUFpkbniCjJagqrQ=',
    'base64',
  );
}

function pngChunk(name: string, body: Uint8Array): Uint8Array {
  const type = new TextEncoder().encode(name);
  const result = new Uint8Array(body.length + 12);
  const view = new DataView(result.buffer);
  view.setUint32(0, body.length);
  result.set(type, 4);
  result.set(body, 8);
  view.setUint32(body.length + 8, crc32(join(type, body)));
  return result;
}

function join(...parts: readonly Uint8Array[]): Uint8Array {
  const result = new Uint8Array(parts.reduce((total, part) => total + part.length, 0));
  let at = 0;
  for (const part of parts) {
    result.set(part, at);
    at += part.length;
  }
  return result;
}

function crc32(bytes: Uint8Array): number {
  let crc = 0xffffffff;
  for (const byte of bytes) {
    crc ^= byte;
    for (let bit = 0; bit < 8; bit += 1) {
      crc = (crc >>> 1) ^ (crc & 1 ? 0xedb88320 : 0);
    }
  }
  return (crc ^ 0xffffffff) >>> 0;
}

function putText(bytes: Uint8Array, at: number, text: string): void {
  for (let index = 0; index < text.length; ++index) {
    bytes[at + index] = text.charCodeAt(index);
  }
}

async function eventually(condition: () => boolean, failure: string): Promise<void> {
  for (let attempt = 0; attempt < 50; ++attempt) {
    if (condition()) {
      return;
    }
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  assert.fail(failure);
}

function requiredEnvironment(name: string): string {
  const value = process.env[name];
  assert.ok(value, `${name} was not supplied to the extension smoke`);
  return value;
}
