import assert from 'node:assert/strict';
import { test } from 'node:test';
import type { EddsConversion, EddsInspection, EddsPreview } from '../../src/mods/edds';
import { DEFAULT_TEXTURE_PROFILE, type TextureProfile } from '../../src/mods/textureConversion';
import {
  textureProfileFieldsOf,
  openedTextureAuthoring,
  updateTextureAuthoring,
} from '../../src/mods/textureAuthoring';

test('load settles into authoring and asks the native pipeline for a temporary preview', () => {
  const opened = openedTextureAuthoring();
  assert.deepEqual(opened, {
    state: { kind: 'loading' },
    effects: [{ kind: 'load' }],
  });

  const loaded = updateTextureAuthoring(opened.state, { kind: 'loaded', plan: plan() });
  assert.equal(loaded.state.kind, 'authoring');
  assert.deepEqual(loaded.effects, [
    { kind: 'render-draft', revision: 1, mip: 0, plan: plan(), profile: DEFAULT_TEXTURE_PROFILE },
  ]);
});

test('mip selection re-renders that level without changing the draft profile', () => {
  const authoring = readyAuthoring();
  const selected = updateTextureAuthoring(authoring, { kind: 'select-mip', mip: 1 });

  assert.equal(selected.state.kind === 'authoring' && selected.state.selectedMip, 1);
  assert.equal(selected.effects[0]?.kind, 'render-draft');
  assert.equal(selected.effects[0]?.kind === 'render-draft' && selected.effects[0].mip, 1);
});

test('profile changes invalidate the old preview and late native results cannot win', () => {
  const loaded = updateTextureAuthoring(openedTextureAuthoring().state, {
    kind: 'loaded',
    plan: plan(),
  });
  const changed = updateTextureAuthoring(loaded.state, {
    kind: 'change-profile',
    field: 'FormatCompress',
    value: 'Best',
  });

  assert.equal(changed.state.kind, 'authoring');
  assert.equal(changed.state.kind === 'authoring' && changed.state.revision, 2);
  assert.equal(
    changed.state.kind === 'authoring' && changed.state.draft.FormatCompress,
    'Best',
  );
  assert.equal(changed.effects[0]?.kind, 'render-draft');

  const stale = updateTextureAuthoring(changed.state, {
    kind: 'draft-rendered',
    revision: 1,
    rendered: rendering(),
  });
  assert.strictEqual(stale.state, changed.state);
});

test('writing the current profile value keeps the clean draft and starts no native work', () => {
  const authoring = readyAuthoring();
  const same = updateTextureAuthoring(authoring, {
    kind: 'change-profile',
    field: 'FormatCompress',
    value: 'Fastest',
  });
  assert.strictEqual(same.state, authoring);
  assert.deepEqual(same.effects, []);
});

test('run captures one immutable profile and locks later property messages', () => {
  const authoring = readyAuthoring();
  const running = updateTextureAuthoring(authoring, { kind: 'run' });

  assert.equal(running.state.kind, 'running');
  assert.deepEqual(running.effects, [
    { kind: 'convert', revision: 1, plan: plan(), profile: DEFAULT_TEXTURE_PROFILE },
  ]);

  const ignored = updateTextureAuthoring(running.state, {
    kind: 'change-profile',
    field: 'GenerateMips',
    value: false,
  });
  assert.strictEqual(ignored.state, running.state);
  assert.deepEqual(ignored.effects, []);
});

test('source or destination revision changes refuse a stale run instead of overwriting', () => {
  const authoring = readyAuthoring();
  for (const artifact of ['source', 'output', 'metadata'] as const) {
    const stale = updateTextureAuthoring(authoring, { kind: 'artifact-changed', artifact });
    assert.deepEqual(stale.state, {
      kind: 'refused',
      reason: `The ${artifact} changed after this conversion session loaded. Reload before writing.`,
    });
    assert.deepEqual(stale.effects, []);
  }
});

test('successful conversion reaches result with actual independently supplied previews', () => {
  const running = updateTextureAuthoring(readyAuthoring(), { kind: 'run' });
  const result = updateTextureAuthoring(running.state, {
    kind: 'converted',
    revision: 1,
    conversion: CONVERSION,
    rendered: rendering(),
  });

  assert.equal(result.state.kind, 'result');
  assert.equal(result.state.kind === 'result' && result.state.conversion.registered, true);
  assert.notStrictEqual(
    result.state.kind === 'result' && result.state.rendered.source,
    result.state.kind === 'result' && result.state.rendered.result,
  );
});

test('every Workbench key is visible while unsupported dependent values explain their lock', () => {
  assert.deepEqual(
    textureProfileFieldsOf(DEFAULT_TEXTURE_PROFILE).map(({ key, editable, reason }) => [
      key,
      editable,
      reason,
    ]),
    [
      ['TargetFormat', false, 'This conversion slice supports EnfusionDDS only.'],
      ['FormatCompress', true, undefined],
      ['CompressTreshold', true, undefined],
      ['Conversion', true, undefined],
      [
        'ConversionQuality',
        false,
        'Conversion=None stores its channels as they are, so quality has nothing to trade.',
      ],
      ['Swizzling', false, 'This conversion slice supports None only.'],
      ['GenerateMips', true, undefined],
      ['MipMapFunction', false, 'GenerateMips uses Filter in this conversion slice.'],
      ['MipMapFilter', false, 'MipMapFunction=Filter uses Box in this conversion slice.'],
      ['TiledTexture', false, 'TiledTexture=false is not supported in this conversion slice.'],
    ],
  );
});

test('a compressed conversion is what makes ConversionQuality a control at all', () => {
  const fields = (conversion: TextureProfile['Conversion']) =>
    textureProfileFieldsOf({ ...DEFAULT_TEXTURE_PROFILE, Conversion: conversion })
      .find((field) => field.key === 'ConversionQuality');

  assert.equal(fields('ColorHQCompression')?.editable, true);
  assert.equal(fields('ColorHQCompression')?.reason, undefined);
  assert.equal(fields('DXTCompression')?.editable, true);
  assert.equal(fields('RedHQCompression')?.editable, true);
  assert.equal(fields('RedGreenHQCompression')?.editable, true);
  for (const uncompressed of ['None', 'Red', 'RedGreen'] as const) {
    assert.equal(fields(uncompressed)?.editable, false);
    assert.match(String(fields(uncompressed)?.reason), /stores its channels as they are/);
  }
});

test('a conversion change that drops quality carries the default back with it', () => {
  const authoring = readyAuthoring();
  const hq = updateTextureAuthoring(authoring, {
    kind: 'change-profile',
    field: 'Conversion',
    value: 'ColorHQCompression',
  });
  const lowered = updateTextureAuthoring(hq.state, {
    kind: 'change-profile',
    field: 'ConversionQuality',
    value: 0.403,
  });
  assert.equal(
    lowered.state.kind === 'authoring' && lowered.state.draft.ConversionQuality,
    0.403,
  );
  const uncompressed = updateTextureAuthoring(lowered.state, {
    kind: 'change-profile',
    field: 'Conversion',
    value: 'Red',
  });
  assert.equal(uncompressed.state.kind === 'authoring' && uncompressed.state.draft.Conversion, 'Red');
  assert.equal(uncompressed.state.kind === 'authoring' && uncompressed.state.draft.ConversionQuality, 1);
});

test('a quality outside the recipe grammar never reaches a draft', () => {
  const hq = updateTextureAuthoring(readyAuthoring(), {
    kind: 'change-profile',
    field: 'Conversion',
    value: 'DXTCompression',
  });
  for (const refused of [-0.1, 1.1, 0.4031, Number.NaN]) {
    const attempt = updateTextureAuthoring(hq.state, {
      kind: 'change-profile',
      field: 'ConversionQuality',
      value: refused,
    });
    assert.equal(attempt.state.kind === 'authoring' && attempt.state.draft.ConversionQuality, 1);
    assert.deepEqual(attempt.effects, []);
  }
});

test('an uncompressed conversion refuses a quality rather than accepting a dead one', () => {
  const attempt = updateTextureAuthoring(readyAuthoring(), {
    kind: 'change-profile',
    field: 'ConversionQuality',
    value: 0.5,
  });
  assert.equal(attempt.state.kind === 'authoring' && attempt.state.draft.ConversionQuality, 1);
  assert.deepEqual(attempt.effects, []);
});

function readyAuthoring() {
  const loaded = updateTextureAuthoring(openedTextureAuthoring().state, {
    kind: 'loaded',
    plan: plan(),
  });
  return updateTextureAuthoring(loaded.state, {
    kind: 'draft-rendered',
    revision: 1,
    rendered: rendering(),
  }).state;
}

function plan() {
  return {
    kind: 'ready' as const,
    scope: 'registered' as const,
    action: 'convert' as const,
    label: 'Convert' as const,
    source: 'C:\\mod\\Mod\\icon.png',
    sourceFormat: 'PNG' as const,
    output: 'C:\\mod\\Mod\\icon.edds',
    metadata: 'C:\\mod\\Mod\\icon.edds.meta',
    identity: { guid: '0123456789ABCDEF', name: 'Mod/icon.edds', sourceFile: 'icon.png' },
    identityAction: 'create' as const,
    profile: DEFAULT_TEXTURE_PROFILE,
    revisions: { source: { size: 12, modified: 34 } },
    notice: undefined,
  };
}

const INSPECTION: EddsInspection = {
  width: 1,
  height: 1,
  pixelFormat: 'BGRA8',
  channels: 'RGBA',
  dds: {
    flags: 1,
    pitchOrLinearSize: 4,
    depth: 0,
    pixelFormatFlags: 65,
    fourCC: 'ENF1',
    rgbBitCount: 32,
    rMask: 0x00ff0000,
    gMask: 0x0000ff00,
    bMask: 0x000000ff,
    aMask: 0xff000000,
    caps: 0x1000,
    caps2: 0,
    dxgiFormat: 0,
    resourceDimension: 0,
    arraySize: 0,
    miscFlag: 0,
  },
  mips: [
    { level: 0, width: 2, height: 2, container: 'COPY', storedBytes: 16, decodedBytes: 16 },
    { level: 1, width: 1, height: 1, container: 'COPY', storedBytes: 4, decodedBytes: 4 },
  ],
  pixels: { kind: 'supported' },
};

const CONVERSION: EddsConversion = {
  width: 1,
  height: 1,
  mipCount: 1,
  pixelFormat: 'BGRA8',
  registered: true,
};

function rendering() {
  const source: EddsPreview = {
    level: 0,
    width: 1,
    height: 1,
    rgba: Uint8Array.from([1, 2, 3, 4]),
  };
  const result: EddsPreview = {
    level: 0,
    width: 1,
    height: 1,
    rgba: Uint8Array.from(source.rgba),
  };
  return { inspection: INSPECTION, source, result };
}
