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
  commands: ['inspect', 'preview'],
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

function inspectMessage(): string {
  return JSON.stringify({
    protocolVersion: 1,
    kind: 'inspect',
    width: 1,
    height: 1,
    mipCount: 1,
    pixelFormat: 'BGRX8',
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
