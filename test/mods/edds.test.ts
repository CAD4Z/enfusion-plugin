import assert from 'node:assert/strict';
import { test } from 'node:test';
import {
  EDDS_PROTOCOL_VERSION,
  inspectionOf,
  machineFailureOf,
  previewOf,
  protocolOf,
} from '../../src/mods/edds';

test('the protocol handshake accepts the converter contract this extension speaks', () => {
  assert.deepEqual(
    protocolOf(
      JSON.stringify({
        protocolVersion: 1,
        kind: 'protocol',
        toolVersion: '0.1.0',
        commands: ['inspect', 'preview'],
      }),
    ),
    {
      protocolVersion: EDDS_PROTOCOL_VERSION,
      toolVersion: '0.1.0',
      commands: ['inspect', 'preview'],
    },
  );
});

test('a different or malformed protocol is refused before its values are used', () => {
  assert.throws(
    () => protocolOf('{"protocolVersion":2,"kind":"protocol","toolVersion":"0.2","commands":[]}'),
    /protocol version 2.*requires 1/i,
  );
  assert.throws(() => protocolOf('not json'), /valid JSON/i);
  assert.throws(
    () =>
      protocolOf(
        '{"protocolVersion":1,"kind":"protocol","toolVersion":"0.1","commands":["inspect"]}',
      ),
    /preview command/i,
  );
});

test('inspect preserves common DDS and every actual ENF1 mip fact', () => {
  const inspection = inspectionOf(
    JSON.stringify({
      protocolVersion: 1,
      kind: 'inspect',
      width: 3,
      height: 2,
      mipCount: 2,
      pixelFormat: 'BGRA8',
      previewSupported: true,
      dds: {
        flags: 0x1_000f,
        pitchOrLinearSize: 12,
        depth: 0,
        pixelFormatFlags: 0x4,
        fourCC: 'ENF1',
        rgbBitCount: 32,
        rMask: 0x00ff_0000,
        gMask: 0x0000_ff00,
        bMask: 0x0000_00ff,
        aMask: 0xff00_0000,
        caps: 0x401008,
        caps2: 0,
        dxgiFormat: 0,
        resourceDimension: 0,
        arraySize: 0,
        miscFlag: 0,
      },
      mips: [
        {
          level: 0,
          width: 3,
          height: 2,
          container: 'COPY',
          storedBytes: 24,
          decodedBytes: 24,
        },
        {
          level: 1,
          width: 1,
          height: 1,
          container: 'LZ4',
          storedBytes: 5,
          decodedBytes: 4,
        },
      ],
    }),
  );

  assert.equal(inspection.pixelFormat, 'BGRA8');
  assert.deepEqual(inspection.pixels, { kind: 'supported' });
  assert.equal(inspection.dds.aMask, 0xff00_0000);
  assert.deepEqual(
    inspection.mips.map((mip) => [mip.level, mip.width, mip.height, mip.container]),
    [
      [0, 3, 2, 'COPY'],
      [1, 1, 1, 'LZ4'],
    ],
  );
});

test('recognized but undecodable pixels remain an honest unsupported-format inspection', () => {
  const source = inspectMessage({ pixelFormat: 'DXGI_98', previewSupported: false });
  const inspection = inspectionOf(JSON.stringify(source));

  assert.deepEqual(inspection.pixels, {
    kind: 'unsupported',
    reason: 'Pixel preview is unavailable because DXGI_98 is not supported.',
  });
});

test('a previewable format with an unsupported DDS topology keeps the converter reason', () => {
  const inspection = inspectionOf(
    JSON.stringify(
      inspectMessage({
        previewSupported: false,
        previewUnsupportedReason: 'Only one two-dimensional texture surface is supported.',
      }),
    ),
  );

  assert.deepEqual(inspection.pixels, {
    kind: 'unsupported',
    reason: 'Only one two-dimensional texture surface is supported.',
  });
});

test('inspect refuses internally contradictory or malformed machine output', () => {
  assert.throws(
    () => inspectionOf(JSON.stringify(inspectMessage({ mipCount: 2 }))),
    /mipCount.*mips/i,
  );
  assert.throws(
    () => inspectionOf(JSON.stringify(inspectMessage({ pixelFormat: 'DXT1', previewSupported: true }))),
    /cannot preview DXT1/i,
  );
  assert.throws(
    () => inspectionOf(JSON.stringify(inspectMessage({ width: -1 }))),
    /width.*positive integer/i,
  );
});

test('preview returns exactly the declared top-to-bottom RGBA bytes', () => {
  const rgba = Uint8Array.from([255, 0, 1, 128, 2, 3, 4, 255]);
  const preview = previewOf(
    JSON.stringify({
      protocolVersion: 1,
      kind: 'preview',
      mip: 1,
      width: 2,
      height: 1,
      pixelFormat: 'RGBA8',
      byteLength: rgba.byteLength,
      pixelsBase64: Buffer.from(rgba).toString('base64'),
    }),
  );

  assert.equal(preview.level, 1);
  assert.deepEqual(preview.rgba, rgba);
});

test('preview refuses truncated, non-base64, and mis-sized pixel output', () => {
  const message = {
    protocolVersion: 1,
    kind: 'preview',
    mip: 0,
    width: 1,
    height: 1,
    pixelFormat: 'RGBA8',
    byteLength: 4,
    pixelsBase64: '/wAA',
  };

  assert.throws(() => previewOf(JSON.stringify(message)), /base64/i);
  assert.throws(
    () => previewOf(JSON.stringify({ ...message, pixelsBase64: '!!!!' })),
    /base64/i,
  );
  assert.throws(
    () => previewOf(JSON.stringify({ ...message, byteLength: 3, pixelsBase64: '/wAA/w==' })),
    /byteLength/i,
  );
});

test('a versioned machine failure can supply the human-facing reason for a stable exit category', () => {
  assert.deepEqual(
    machineFailureOf(
      '{"protocolVersion":1,"kind":"error","error":{"category":"invalid-input","code":"truncated-header","message":"DDS header ends at byte 80."}}',
    ),
    {
      category: 'invalid-input',
      code: 'truncated-header',
      message: 'DDS header ends at byte 80.',
    },
  );
});

function inspectMessage(changes: Record<string, unknown>): Record<string, unknown> {
  return {
    protocolVersion: 1,
    kind: 'inspect',
    width: 1,
    height: 1,
    mipCount: 1,
    pixelFormat: 'BGRX8',
    previewSupported: true,
    dds: {
      flags: 0x1_000f,
      pitchOrLinearSize: 4,
      depth: 0,
      pixelFormatFlags: 0x4,
      fourCC: 'ENF1',
      rgbBitCount: 32,
      rMask: 0x00ff_0000,
      gMask: 0x0000_ff00,
      bMask: 0x0000_00ff,
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
    ...changes,
  };
}
