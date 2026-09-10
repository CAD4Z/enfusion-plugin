import assert from 'node:assert/strict';
import { test } from 'node:test';
import type { EddsConversion, EddsInspection, EddsPreview } from '../../src/mods/edds';
import { DEFAULT_TEXTURE_PROFILE } from '../../src/mods/textureConversion';
import {
  TEXTURE_PROFILE_FIELDS,
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
    TEXTURE_PROFILE_FIELDS.map(({ key, editable, reason }) => [key, editable, reason]),
    [
      ['TargetFormat', false, 'The first conversion slice supports EnfusionDDS only.'],
      ['FormatCompress', true, undefined],
      ['CompressTreshold', true, undefined],
      ['Conversion', false, 'The first conversion slice supports None only.'],
      ['ConversionQuality', false, 'Conversion=None fixes ConversionQuality at 1.'],
      ['Swizzling', false, 'The first conversion slice supports None only.'],
      ['GenerateMips', true, undefined],
      ['MipMapFunction', false, 'GenerateMips uses Filter in the first conversion slice.'],
      ['MipMapFilter', false, 'MipMapFunction=Filter uses Box in the first conversion slice.'],
      ['TiledTexture', false, 'TiledTexture=false is not supported in the first conversion slice.'],
    ],
  );
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
