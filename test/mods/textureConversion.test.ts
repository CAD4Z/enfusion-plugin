import assert from 'node:assert/strict';
import { test } from 'node:test';
import {
  DEFAULT_TEXTURE_PROFILE,
  type TextureConversionInput,
  textureConversionPlanOf,
  textureGuidOf,
} from '../../src/mods/textureConversion';

const sourceRevision = { size: 91, modified: 1_725_000_000_000 };
const outputRevision = { size: 412, modified: 1_725_000_001_000 };
const metadataRevision = { size: 730, modified: 1_725_000_002_000 };

test('a registered new texture is planned whole, including identity and revisions', () => {
  assert.deepEqual(textureConversionPlanOf(input()), {
    kind: 'ready',
    scope: 'registered',
    action: 'convert',
    label: 'Convert',
    source: 'C:\\repo\\MyMod\\MyMod\\GUI\\icon.png',
    sourceFormat: 'PNG',
    output: 'C:\\repo\\MyMod\\MyMod\\GUI\\icon.edds',
    metadata: 'C:\\repo\\MyMod\\MyMod\\GUI\\icon.edds.meta',
    identity: {
      guid: '0123456789ABCDEF',
      name: 'MyMod/GUI/icon.edds',
      sourceFile: 'icon.png',
    },
    identityAction: 'create',
    profile: DEFAULT_TEXTURE_PROFILE,
    revisions: { source: sourceRevision },
    notice: undefined,
  });
});

test('the same owned source reconverts and preserves the GUID character for character', () => {
  assert.deepEqual(
    textureConversionPlanOf(
      input({
        outputRevision,
        metadata: {
          kind: 'valid',
          revision: metadataRevision,
          value: metadata('aBcDeF0123456789', 'icon.png'),
        },
      }),
    ),
    {
      kind: 'ready',
      scope: 'registered',
      action: 'reconvert',
      label: 'Reconvert',
      source: 'C:\\repo\\MyMod\\MyMod\\GUI\\icon.png',
      sourceFormat: 'PNG',
      output: 'C:\\repo\\MyMod\\MyMod\\GUI\\icon.edds',
      metadata: 'C:\\repo\\MyMod\\MyMod\\GUI\\icon.edds.meta',
      identity: {
        guid: 'aBcDeF0123456789',
        name: 'MyMod/GUI/icon.edds',
        sourceFile: 'icon.png',
      },
      identityAction: 'preserve',
      profile: DEFAULT_TEXTURE_PROFILE,
      revisions: {
        source: sourceRevision,
        output: outputRevision,
        metadata: metadataRevision,
      },
      notice: undefined,
    },
  );
});

test('taking an owned EDDS over from another source is an explicit replacement with its GUID', () => {
  const plan = textureConversionPlanOf(
    input({
      outputRevision,
      metadata: {
        kind: 'valid',
        revision: metadataRevision,
        value: metadata('A0A1A2A3A4A5A6A7', 'old.tga'),
      },
    }),
  );

  assert.equal(plan.kind, 'ready');
  assert.equal(plan.action, 'replace');
  assert.equal(plan.label, 'Replace');
  assert.equal(plan.identity?.guid, 'A0A1A2A3A4A5A6A7');
  assert.deepEqual(plan.profile, DEFAULT_TEXTURE_PROFILE);
});

test('filename suffixes never select a hidden profile', () => {
  for (const suffix of ['_ca', '_nohq', '_smdi']) {
    const plan = textureConversionPlanOf(
      input({ source: `C:\\repo\\MyMod\\MyMod\\GUI\\icon${suffix}.png` }),
    );
    assert.equal(plan.kind, 'ready');
    assert.deepEqual(plan.kind === 'ready' && plan.profile, DEFAULT_TEXTURE_PROFILE);
  }
});

test('a standalone EDDS is replaced with a new collision-checked GUID', () => {
  const plan = textureConversionPlanOf(input({ outputRevision }));

  assert.equal(plan.kind, 'ready');
  assert.equal(plan.action, 'replace');
  assert.equal(plan.identity?.guid, '0123456789ABCDEF');
});

test('inside an Enfusion root but outside a prefix is detached and never authors metadata', () => {
  assert.deepEqual(
    textureConversionPlanOf(
      input({
        source: 'C:\\repo\\loose.tga',
        roots: [{ root: 'C:\\repo', prefixRoot: 'C:\\repo\\MyMod\\MyMod' }],
      }),
    ),
    {
      kind: 'ready',
      scope: 'detached',
      action: 'convert',
      label: 'Convert',
      source: 'C:\\repo\\loose.tga',
      sourceFormat: 'TGA',
      output: 'C:\\repo\\loose.edds',
      metadata: undefined,
      identity: undefined,
      identityAction: 'none',
      profile: DEFAULT_TEXTURE_PROFILE,
      revisions: { source: sourceRevision },
      notice: 'Registration was skipped because the source is outside an Enfusion prefix root.',
    },
  );
});

test('detached output with metadata is refused instead of guessing its ownership', () => {
  assert.deepEqual(
    textureConversionPlanOf(
      input({
        source: 'C:\\repo\\loose.png',
        roots: [{ root: 'C:\\repo' }],
        metadata: {
          kind: 'valid',
          revision: metadataRevision,
          value: metadata('0123456789ABCDEF', 'loose.png'),
        },
      }),
    ),
    {
      kind: 'refused',
      reason: 'A detached conversion cannot replace an EDDS that has registration metadata.',
    },
  );
});

test('files outside every discovered Enfusion root are not conversion targets', () => {
  assert.deepEqual(
    textureConversionPlanOf(input({ source: 'C:\\elsewhere\\icon.png' })),
    {
      kind: 'refused',
      reason: 'The source is outside every discovered Enfusion root.',
    },
  );
});

test('malformed metadata is refused before preview or write', () => {
  assert.deepEqual(
    textureConversionPlanOf(
      input({
        metadata: {
          kind: 'invalid',
          revision: metadataRevision,
          reason: 'Conversion DXT5 is not supported.',
        },
      }),
    ),
    {
      kind: 'refused',
      reason: 'The existing metadata is not safe to replace: Conversion DXT5 is not supported.',
    },
  );
});

test('a generated GUID retries collisions and is uppercase 64-bit hexadecimal', () => {
  const values = [
    Uint8Array.from([0, 1, 2, 3, 4, 5, 6, 7]),
    Uint8Array.from([255, 238, 221, 204, 187, 170, 153, 136]),
  ];

  assert.equal(textureGuidOf(() => values.shift()!, ['0001020304050607']), 'FFEEDDCCBBAA9988');
});

function input(over: Partial<TextureConversionInput> = {}): TextureConversionInput {
  return {
    source: 'C:\\repo\\MyMod\\MyMod\\GUI\\icon.png',
    roots: [{ root: 'C:\\repo\\MyMod', prefixRoot: 'C:\\repo\\MyMod\\MyMod' }],
    sourceRevision,
    outputRevision: undefined,
    metadata: { kind: 'missing' },
    newGuid: '0123456789ABCDEF',
    occupiedGuids: [],
    ...over,
  };
}

function metadata(guid: string, sourceFile: string) {
  return {
    guid,
    name: 'MyMod/GUI/icon.edds',
    sourceFile,
    sourceFormat: 'PNG' as const,
    profile: DEFAULT_TEXTURE_PROFILE,
  };
}
