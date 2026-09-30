import assert from 'node:assert/strict';
import { test } from 'node:test';
import type { EddsConversion, EddsInspection, EddsPreview } from '../../src/mods/edds';
import { DEFAULT_TEXTURE_PROFILE, type TextureProfile } from '../../src/mods/textureConversion';
import { TEXTURE_SWIZZLES } from '../../src/mods/textureSwizzles';
import {
  textureProfileFieldsOf,
  openedTextureAuthoring,
  updateTextureAuthoring,
} from '../../src/mods/textureAuthoring';

test('an explicit swizzle invalidates the result and reaches the conversion profile unchanged', () => {
  const initial = readyAuthoring();
  const changed = updateTextureAuthoring(initial, {
    kind: 'change-profile', field: 'Swizzling', value: 'NormalMap_NOHQ',
  });
  const profile = { ...DEFAULT_TEXTURE_PROFILE, Swizzling: 'NormalMap_NOHQ' };
  assert.deepEqual(changed.effects, [{
    kind: 'render-draft', revision: 2, mip: 0,
    plan: { ...plan(), profile }, profile,
  }]);
  assert.deepEqual(textureProfileFieldsOf(DEFAULT_TEXTURE_PROFILE).find(({ key }) => key === 'Swizzling'), {
    key: 'Swizzling', editable: true,
  });
});

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

test('every explicit swizzle reaches the preview and terrain settings cannot disable required mips', () => {
  for (const { name } of TEXTURE_SWIZZLES.slice(1)) {
    const changed = updateTextureAuthoring(readyAuthoring(), { kind: 'change-profile', field: 'Swizzling', value: name });
    assert.equal(changed.effects[0]?.kind === 'render-draft' && changed.effects[0].profile.Swizzling, name);
    if (name !== 'TerrainLayerTexture' && name !== 'TerrainSuperTexture') continue;
    const disabled = updateTextureAuthoring(changed.state, { kind: 'change-profile', field: 'GenerateMips', value: false });
    assert.strictEqual(disabled.state, changed.state);
    for (const field of textureProfileFieldsOf({ ...DEFAULT_TEXTURE_PROFILE, Swizzling: name }, 'DDS')) {
      if (field.key !== 'GenerateMips' && field.key !== 'ContainsMips') continue;
      assert.equal(field.editable, false);
      assert.match(field.reason ?? '', /Terrain/);
    }
  }
});

test('known RGB source facts disable ambient mip removal with an explanation', () => {
  const facts = { width: 16, height: 16, hasAlpha: false };
  const profile = { ...DEFAULT_TEXTURE_PROFILE, Swizzling: 'AmbientSpecularMapGA' } as const;
  const field = textureProfileFieldsOf(profile, 'TGA', facts).find(({ key }) => key === 'RemoveMips');
  assert.equal(field?.editable, false);
  assert.match(field?.reason ?? '', /alpha channel/);
  const loaded = updateTextureAuthoring(readyAuthoring(), {
    kind: 'draft-rendered', revision: 1, rendered: { ...rendering(), sourceFacts: facts },
  });
  const changed = updateTextureAuthoring(loaded.state, { kind: 'change-profile', field: 'Swizzling', value: 'AmbientSpecularMapGA' });
  const refused = updateTextureAuthoring(changed.state, { kind: 'change-profile', field: 'RemoveMips', value: 1 });
  assert.strictEqual(refused.state, changed.state);
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
      ['RemoveMips', true, undefined],
      ['Conversion', true, undefined],
      [
        'ConversionQuality',
        false,
        'Conversion=None stores its channels as they are, so quality has nothing to trade.',
      ],
      ['Swizzling', true, undefined],
      ['ContainsMips', false, 'ContainsMips is available only for a DDS source.'],
      ['GenerateMips', true, undefined],
      ['Normalize', true, undefined],
      ['MipMapFunction', true, undefined],
      ['MipMapFilter', true, undefined],
      ['TiledTexture', true, undefined],
    ],
  );
});

test('a DDS supplied-chain choice disables generation and its dependent filter controls', () => {
  const loaded = updateTextureAuthoring(openedTextureAuthoring().state, {
    kind: 'loaded',
    plan: plan('DDS'),
  });
  const filtered = updateTextureAuthoring(loaded.state, {
    kind: 'change-profile', field: 'MipMapFilter', value: 'Kaiser',
  });
  const changed = updateTextureAuthoring(filtered.state, {
    kind: 'change-profile',
    field: 'ContainsMips',
    value: true,
  });
  assert.equal(changed.state.kind, 'authoring');
  const draft = changed.state.kind === 'authoring' ? changed.state.draft : DEFAULT_TEXTURE_PROFILE;
  assert.equal(draft.ContainsMips, true);
  assert.equal(draft.GenerateMips, false);
  assert.equal(draft.MipMapFunction, 'Filter');
  assert.equal(draft.MipMapFilter, 'Box');
  assert.deepEqual(
    textureProfileFieldsOf(draft, 'DDS')
      .filter(({ key }) => key === 'ContainsMips' || key === 'GenerateMips' || key === 'MipMapFilter')
      .map(({ key, editable, reason }) => [key, editable, reason]),
    [
      ['ContainsMips', true, undefined],
      ['GenerateMips', false, 'GenerateMips is disabled while ContainsMips supplies the chain.'],
      ['MipMapFilter', false, 'MipMapFilter applies only while GenerateMips is enabled.'],
    ],
  );
});

test('turning off mip generation or its filter stage resets inactive settings', () => {
  const filtered = updateTextureAuthoring(readyAuthoring(), {
    kind: 'change-profile', field: 'MipMapFilter', value: 'Kaiser',
  });
  const normalized = updateTextureAuthoring(filtered.state, {
    kind: 'change-profile', field: 'MipMapFunction', value: 'Normalize',
  });
  assert.equal(
    normalized.state.kind === 'authoring' && normalized.state.draft.MipMapFilter,
    'Box',
  );

  const disabled = updateTextureAuthoring(normalized.state, {
    kind: 'change-profile', field: 'GenerateMips', value: false,
  });
  assert.equal(disabled.state.kind === 'authoring' && disabled.state.draft.MipMapFunction, 'Filter');
  assert.equal(disabled.state.kind === 'authoring' && disabled.state.draft.MipMapFilter, 'Box');
});

test('ColorNoise keeps Kaiser active and untiled borders reach the draft preview', () => {
  const filtered = updateTextureAuthoring(readyAuthoring(), {
    kind: 'change-profile', field: 'MipMapFilter', value: 'Kaiser',
  });
  const colored = updateTextureAuthoring(filtered.state, {
    kind: 'change-profile', field: 'MipMapFunction', value: 'ColorNoise',
  });
  const untiled = updateTextureAuthoring(colored.state, {
    kind: 'change-profile', field: 'TiledTexture', value: false,
  });
  assert.equal(untiled.state.kind, 'authoring');
  if (untiled.state.kind !== 'authoring') return;
  assert.equal(untiled.state.draft.MipMapFunction, 'ColorNoise');
  assert.equal(untiled.state.draft.MipMapFilter, 'Kaiser');
  assert.equal(untiled.state.draft.TiledTexture, false);
  assert.equal(textureProfileFieldsOf(untiled.state.draft)
    .find(({ key }) => key === 'MipMapFilter')?.editable, true);
  assert.equal(untiled.effects[0]?.kind, 'render-draft');
  if (untiled.effects[0]?.kind !== 'render-draft') return;
  assert.deepEqual(untiled.effects[0].profile, untiled.state.draft);
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

function plan(sourceFormat: 'PNG' | 'DDS' = 'PNG') {
  return {
    kind: 'ready' as const,
    scope: 'registered' as const,
    action: 'convert' as const,
    label: 'Convert' as const,
    source: 'C:\\mod\\Mod\\icon.png',
    sourceFormat,
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
