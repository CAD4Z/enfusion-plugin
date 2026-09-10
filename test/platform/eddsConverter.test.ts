import assert from 'node:assert/strict';
import path from 'node:path';
import { test } from 'node:test';
import {
  type ExecutableRequest,
  EddsConverter,
  EddsConverterError,
} from '../../src/platform/eddsConverter';

const PROTOCOL = JSON.stringify({
  protocolVersion: 1,
  kind: 'protocol',
  toolVersion: '0.1.0',
  commands: ['inspect', 'preview', 'convert', 'batch'],
});

test('the adapter handshakes once and invokes only its installed executable without a shell', async () => {
  const calls: ExecutableRequest[] = [];
  const execute = (request: ExecutableRequest): Promise<{ stdout: string; stderr: string }> => {
    calls.push(request);
    if (request.args[0] === 'protocol') {
      return Promise.resolve({ stdout: PROTOCOL, stderr: '' });
    }
    return Promise.resolve({ stdout: inspectMessage(), stderr: '' });
  };
  const converter = new EddsConverter('C:\\extension', execute);

  await converter.inspect('D:\\loose file.edds');
  await converter.inspect('D:\\another.edds');

  assert.equal(calls.length, 3);
  assert.equal(
    calls[0]?.executable,
    path.join('C:\\extension', 'dist', 'native', 'win32-x64', 'edds-convert.exe'),
  );
  assert.deepEqual(calls[1]?.args, [
    'inspect',
    '--machine',
    '--protocol',
    '1',
    '--input',
    'D:\\loose file.edds',
  ]);
  assert.equal(calls.every((call) => call.shell === false), true);
});

test('machine failures retain the stable native category and useful diagnostics', async () => {
  const converter = new EddsConverter('C:\\extension', (request) => {
    if (request.args[0] === 'protocol') {
      return Promise.resolve({ stdout: PROTOCOL, stderr: '' });
    }
    return Promise.reject(
      Object.assign(new Error('converter exit 4'), {
        code: 4,
        stdout:
          '{"protocolVersion":1,"kind":"error","error":{"category":"unsupported-format","code":"unsupported-pixel-format","message":"DXT1 pixels cannot be previewed."}}',
        stderr: 'edds-convert: unsupported-format: DXT1 pixels cannot be previewed.',
      }),
    );
  });

  await assert.rejects(converter.preview('D:\\texture.edds', 0), (error: unknown) => {
    assert.ok(error instanceof EddsConverterError);
    assert.equal(error.category, 'unsupported-format');
    assert.equal(error.nativeCode, 'unsupported-pixel-format');
    assert.match(error.message, /DXT1 pixels/);
    return true;
  });
});

test('conversion spells out the immutable plan through stable native flags', async () => {
  const calls: ExecutableRequest[] = [];
  const converter = new EddsConverter('C:\\extension', (request) => {
    calls.push(request);
    return Promise.resolve({
      stdout:
        request.args[0] === 'protocol'
          ? PROTOCOL
          : '{"protocolVersion":1,"kind":"convert","width":3,"height":2,"mipCount":2,"pixelFormat":"BGRA8","registered":true}',
      stderr: '',
    });
  });

  const result = await converter.convert({
    kind: 'ready',
    scope: 'registered',
    action: 'replace',
    label: 'Replace',
    source: 'C:\\mod\\Mod\\icon.png',
    sourceFormat: 'PNG',
    output: 'C:\\mod\\Mod\\icon.edds',
    metadata: 'C:\\mod\\Mod\\icon.edds.meta',
    identity: { guid: '0123456789ABCDEF', name: 'Mod/icon.edds', sourceFile: 'icon.png' },
    identityAction: 'preserve',
    profile: {
      TargetFormat: 'EnfusionDDS',
      FormatCompress: 'Medium',
      CompressTreshold: 80,
      Conversion: 'None',
      ConversionQuality: 1,
      Swizzling: 'None',
      GenerateMips: false,
      MipMapFunction: 'Filter',
      MipMapFilter: 'Box',
      TiledTexture: true,
    },
    revisions: { source: { size: 1, modified: 2 } },
  });

  assert.equal(result.registered, true);
  assert.deepEqual(calls[1]?.args, [
    'convert', '--machine', '--protocol', '1',
    '--input', 'C:\\mod\\Mod\\icon.png',
    '--output', 'C:\\mod\\Mod\\icon.edds',
    '--target-format', 'enfusion-dds',
    '--format-compress', 'medium',
    '--compress-threshold', '80',
    '--conversion', 'none',
    '--conversion-quality', '1',
    '--swizzling', 'none',
    '--generate-mips', 'false',
    '--mipmap-function', 'filter',
    '--mipmap-filter', 'box',
    '--tiled-texture', 'true',
    '--expect-source-revision', '1:2',
    '--expect-output-revision', 'missing',
    '--expect-metadata-revision', 'missing',
    '--metadata', 'C:\\mod\\Mod\\icon.edds.meta',
    '--resource-name', 'Mod/icon.edds',
    '--source-file', 'icon.png',
    '--guid', '0123456789ABCDEF',
  ]);
});

function inspectMessage(): string {
  return JSON.stringify({
    protocolVersion: 1,
    kind: 'inspect',
    width: 1,
    height: 1,
    mipCount: 1,
    pixelFormat: 'BGRX8',
    channels: 'RGB',
    previewSupported: true,
    dds: {
      flags: 1,
      pitchOrLinearSize: 4,
      depth: 0,
      pixelFormatFlags: 65,
      fourCC: 'NONE',
      rgbBitCount: 32,
      rMask: 0x00ff0000,
      gMask: 0x0000ff00,
      bMask: 0x000000ff,
      aMask: 0,
      caps: 0x1000,
      caps2: 0,
      dxgiFormat: 0,
      resourceDimension: 0,
      arraySize: 0,
      miscFlag: 0,
    },
    mips: [
      {
        level: 0,
        width: 1,
        height: 1,
        container: 'COPY',
        storedBytes: 4,
        decodedBytes: 4,
      },
    ],
  });
}
