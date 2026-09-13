import assert from 'node:assert/strict';
import { test } from 'node:test';
import {
  EDDS_PROTOCOL_VERSION,
  conversionOf,
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
        commands: ['inspect', 'preview', 'convert', 'batch'],
      }),
    ),
    {
      protocolVersion: EDDS_PROTOCOL_VERSION,
      toolVersion: '0.1.0',
      commands: ['inspect', 'preview', 'convert', 'batch'],
    },
  );
});

test('inspect accepts only the structured supported metadata recipe', () => {
  const inspection = inspectionOf(
    JSON.stringify({
      ...inspectMessage({}),
      metadata: {
        schemaVersion: 1,
        identity: {
          guid: 'aBcDeF0123456789',
          name: 'MyMod/GUI/icon.edds',
          sourceFile: 'icon.png',
          sourceFormat: 'png',
        },
        recipe: {
          TargetFormat: 'EnfusionDDS',
          FormatCompress: 'Best',
          CompressTreshold: 72,
          Conversion: 'None',
          ConversionQuality: 1,
          Swizzling: 'None',
          GenerateMips: false,
          MipMapFunction: 'Filter',
          MipMapFilter: 'Box',
          TiledTexture: true,
        },
      },
    }),
  );

  assert.deepEqual(inspection.metadata, {
    guid: 'aBcDeF0123456789',
    name: 'MyMod/GUI/icon.edds',
    sourceFile: 'icon.png',
    sourceFormat: 'PNG',
    profile: {
      TargetFormat: 'EnfusionDDS',
      FormatCompress: 'Best',
      CompressTreshold: 72,
      Conversion: 'None',
      ConversionQuality: 1,
      Swizzling: 'None',
      GenerateMips: false,
      MipMapFunction: 'Filter',
      MipMapFilter: 'Box',
      TiledTexture: true,
    },
  });
  assert.throws(
    () =>
      inspectionOf(
        JSON.stringify({
          ...inspectMessage({}),
          metadata: {
            schemaVersion: 1,
            identity: {
              guid: 'short',
              name: 'x.edds',
              sourceFile: 'x.png',
              sourceFormat: 'png',
            },
            recipe: {},
          },
        }),
      ),
    /guid/i,
  );
});

test('every source format in the contract crosses the process boundary, and no alias does', () => {
  for (const [wire, format] of [['jpg', 'JPG'], ['tiff', 'TIFF']] as const) {
    const inspection = inspectionOf(
      JSON.stringify({
        ...inspectMessage({}),
        metadata: {
          schemaVersion: 1,
          identity: {
            guid: 'aBcDeF0123456789',
            name: 'MyMod/GUI/icon.edds',
            sourceFile: `icon.${wire}`,
            sourceFormat: wire,
          },
          recipe: {
            TargetFormat: 'EnfusionDDS',
            FormatCompress: 'Fastest',
            CompressTreshold: 80,
            Conversion: 'None',
            ConversionQuality: 1,
            Swizzling: 'None',
            GenerateMips: true,
            MipMapFunction: 'Filter',
            MipMapFilter: 'Box',
            TiledTexture: true,
          },
        },
      }),
    );
    assert.equal(inspection.metadata?.sourceFormat, format);
  }
  for (const alias of ['jpeg', 'tif']) {
    assert.throws(
      () =>
        inspectionOf(
          JSON.stringify({
            ...inspectMessage({}),
            metadata: {
              schemaVersion: 1,
              identity: {
                guid: 'aBcDeF0123456789',
                name: 'x.edds',
                sourceFile: `x.${alias}`,
                sourceFormat: alias,
              },
              recipe: {},
            },
          }),
        ),
      /sourceFormat/,
    );
  }
});

test('identity-only inspect keeps a valid GUID when the old recipe is unsupported', () => {
  const value = inspectionOf(JSON.stringify({
    ...inspectMessage({}),
    unsupportedMetadata: {
      reason: 'Workbench setting Conversion=DXTCompression is recognized but unsupported.',
      identity: {
        guid: 'aBcDeF0123456789',
        name: 'Mod/icon.edds',
        sourceFile: 'icon.png',
      },
    },
  }));

  assert.deepEqual(value.unsupportedMetadata, {
    reason: 'Workbench setting Conversion=DXTCompression is recognized but unsupported.',
    identity: {
      guid: 'aBcDeF0123456789',
      name: 'Mod/icon.edds',
      sourceFile: 'icon.png',
    },
  });
  assert.equal(value.metadata, undefined);
});

test('convert returns only actual committed artifact facts', () => {
  assert.deepEqual(
    conversionOf(
      '{"protocolVersion":1,"kind":"convert","width":3,"height":2,"mipCount":2,"pixelFormat":"BGRA8","registered":true}',
    ),
    { width: 3, height: 2, mipCount: 2, pixelFormat: 'BGRA8', registered: true },
  );
});

test('every runtime format a supported conversion produces comes back as a fact', () => {
  for (const format of ['BGRA8', 'BGRX8', 'R8', 'RG8', 'DXT1', 'DXT5', 'BC4', 'BC5', 'BC7']) {
    assert.equal(
      conversionOf(
        `{"protocolVersion":1,"kind":"convert","width":3,"height":2,"mipCount":2,"pixelFormat":"${format}","registered":false}`,
      ).pixelFormat,
      format,
    );
  }
  assert.throws(
    () =>
      conversionOf(
        '{"protocolVersion":1,"kind":"convert","width":3,"height":2,"mipCount":2,"pixelFormat":"DXGI_10","registered":false}',
      ),
    /convert.pixelFormat must be one of/i,
  );
});

test('inspect reports the channels the file holds, including one and two of them', () => {
  for (const [pixelFormat, channels] of [
    ['R8', 'R'],
    ['BC4', 'R'],
    ['RG8', 'RG'],
    ['BC5', 'RG'],
    ['DXT1', 'RGB'],
    ['BC7', 'RGBA'],
  ] as const) {
    const inspection = inspectionOf(JSON.stringify(inspectMessage({ pixelFormat, channels })));
    assert.equal(inspection.pixelFormat, pixelFormat);
    assert.equal(inspection.channels, channels);
    assert.deepEqual(inspection.pixels, { kind: 'supported' });
  }
  assert.throws(
    () => inspectionOf(JSON.stringify(inspectMessage({ channels: 'RA' }))),
    /channels is not recognized/i,
  );
});

test('a GPU recipe round-trips through the machine boundary with its exact quality', () => {
  const recipeOf = (Conversion: string, ConversionQuality: unknown) =>
    JSON.stringify({
      ...inspectMessage({}),
      metadata: {
        schemaVersion: 1,
        identity: {
          guid: 'aBcDeF0123456789',
          name: 'MyMod/GUI/icon.edds',
          sourceFile: 'icon.png',
          sourceFormat: 'png',
        },
        recipe: {
          TargetFormat: 'EnfusionDDS',
          FormatCompress: 'Fastest',
          CompressTreshold: 80,
          Conversion,
          ConversionQuality,
          Swizzling: 'None',
          GenerateMips: true,
          MipMapFunction: 'Filter',
          MipMapFilter: 'Box',
          TiledTexture: true,
        },
      },
    });

  const inspection = inspectionOf(recipeOf('ColorHQCompression', 0.403));
  assert.equal(inspection.metadata?.profile.Conversion, 'ColorHQCompression');
  assert.equal(inspection.metadata?.profile.ConversionQuality, 0.403);

  assert.throws(
    () => inspectionOf(recipeOf('HDRCompression', 1)),
    /Conversion is not supported: HDRCompression/i,
  );
  assert.throws(
    () => inspectionOf(recipeOf('DXTCompression', 1.5)),
    /ConversionQuality must be 0 through 1/i,
  );
  assert.throws(
    () => inspectionOf(recipeOf('DXTCompression', 0.4031)),
    /ConversionQuality must be 0 through 1/i,
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
      channels: 'RGBA',
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
  assert.equal(inspection.channels, 'RGBA');
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
    () => inspectionOf(JSON.stringify(inspectMessage({ pixelFormat: 'DXGI_10', previewSupported: true }))),
    /cannot preview DXGI_10/i,
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
    channels: 'RGB',
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
