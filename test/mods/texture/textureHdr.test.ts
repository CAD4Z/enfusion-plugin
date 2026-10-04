import test from 'node:test';
import assert from 'node:assert/strict';
import { DEFAULT_TEXTURE_PROFILE } from '../../../src/mods/texture/textureConversion';
import { textureSourceFormatOf } from '../../../src/mods/texture/textureSources';
import { textureHdrRefusalOf } from '../../../src/mods/texture/textureHdr';
import { textureProfileFieldsOf } from '../../../src/mods/texture/textureAuthoring';

test('HDR capabilities admit only float conversions and captured panorama topology', () => {
  assert.equal(textureSourceFormatOf('sky.HDR'), 'HDR');
  const profile = { ...DEFAULT_TEXTURE_PROFILE, GenerateCubemap: true };
  assert.equal(textureHdrRefusalOf(profile, 'HDR', { width: 16, height: 8 }), undefined);
  assert.match(textureHdrRefusalOf(profile, 'HDR', { width: 16, height: 16 }) ?? '', /2:1/);
  assert.match(textureHdrRefusalOf(profile, 'PNG') ?? '', /HDR source/);
  assert.match(textureHdrRefusalOf({ ...profile, Conversion: 'ColorHQCompression' }, 'HDR') ?? '', /None or HDRCompression/);
  assert.match(textureHdrRefusalOf({ ...profile, Swizzling: 'AlphaToRGB' }, 'HDR') ?? '', /swizzling/);
  assert.equal(textureHdrRefusalOf({ ...profile, Conversion: 'HDRCompression', ConversionQuality: 0.403 }, 'HDR'), undefined);
});

test('HDR-dependent controls carry a refusal and LDR cannot generate a cube', () => {
  const fields = textureProfileFieldsOf(DEFAULT_TEXTURE_PROFILE, 'HDR');
  assert.equal(fields.find((field) => field.key === 'GenerateCubemap')?.editable, true);
  for (const key of ['Swizzling', 'Normalize', 'MipMapFunction']) {
    assert.equal(fields.find((field) => field.key === key)?.editable, false);
    assert.ok(fields.find((field) => field.key === key)?.reason);
  }
  assert.equal(textureProfileFieldsOf(DEFAULT_TEXTURE_PROFILE, 'PNG')
    .find((field) => field.key === 'GenerateCubemap')?.editable, false);
  const mixed = textureProfileFieldsOf(DEFAULT_TEXTURE_PROFILE, ['HDR', 'PNG']);
  for (const key of ['GenerateCubemap', 'Swizzling', 'Normalize', 'MipMapFunction']) {
    assert.equal(mixed.find((field) => field.key === key)?.editable, false);
    assert.ok(mixed.find((field) => field.key === key)?.reason);
  }
  assert.equal(textureHdrRefusalOf(DEFAULT_TEXTURE_PROFILE, ['HDR', 'PNG']), undefined);
  assert.ok(textureHdrRefusalOf({ ...DEFAULT_TEXTURE_PROFILE, Conversion: 'HDRCompression' }, ['HDR', 'PNG']));
  assert.ok(textureHdrRefusalOf({ ...DEFAULT_TEXTURE_PROFILE, Conversion: 'ColorHQCompression' }, ['HDR', 'PNG']));
});
