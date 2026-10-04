import assert from 'node:assert/strict';
import { test } from 'node:test';
import {
  SUPPORTED_TEXTURE_CONVERSIONS,
  TEXTURE_CONVERSIONS,
  isSupportedTextureConversion,
  isTextureQuality,
  textureChannelViewsOf,
  textureConversionCapabilityOf,
  textureQualityText,
} from '../../../src/mods/texture/textureConversions';

test('the conversion contract names every Workbench value exactly once', () => {
  assert.deepEqual(
    TEXTURE_CONVERSIONS.map((conversion) => conversion.name),
    [
      'None',
      'DXTCompression',
      'Red',
      'RedHQCompression',
      'RedGreen',
      'RedGreenHQCompression',
      'ColorHQCompression',
      'HDRCompression',
    ],
  );
  assert.equal(new Set(TEXTURE_CONVERSIONS.map((row) => row.wire)).size, TEXTURE_CONVERSIONS.length);
  for (const conversion of TEXTURE_CONVERSIONS) {
    assert.equal(textureConversionCapabilityOf(conversion.name), conversion);
  }
});

test('HDRCompression produces unsigned BC6H through the HDR source capability', () => {
  const hdr = textureConversionCapabilityOf('HDRCompression');
  assert.equal(hdr?.supported, true);
  assert.deepEqual(hdr?.formats, ['BC6H']);
  assert.equal(isSupportedTextureConversion('HDRCompression'), true);
  assert.equal(SUPPORTED_TEXTURE_CONVERSIONS.length, 8);
});

test('each conversion carries the runtime formats DayZ writes for it', () => {
  const formats = (name: Parameters<typeof textureConversionCapabilityOf>[0]) =>
    textureConversionCapabilityOf(name)?.formats;
  assert.deepEqual(formats('None'), ['BGRX8', 'BGRA8', 'RGBA32F']);
  assert.deepEqual(formats('DXTCompression'), ['DXT1', 'DXT5']);
  assert.deepEqual(formats('Red'), ['R8']);
  assert.deepEqual(formats('RedHQCompression'), ['BC4']);
  assert.deepEqual(formats('RedGreen'), ['RG8']);
  assert.deepEqual(formats('RedGreenHQCompression'), ['BC5']);
  assert.deepEqual(formats('ColorHQCompression'), ['BC7']);
});

test('only a compressed conversion reads a quality', () => {
  const quality = (name: Parameters<typeof textureConversionCapabilityOf>[0]) =>
    textureConversionCapabilityOf(name)?.usesQuality;
  assert.equal(quality('None'), false);
  assert.equal(quality('Red'), false);
  assert.equal(quality('RedGreen'), false);
  assert.equal(quality('DXTCompression'), true);
  assert.equal(quality('RedHQCompression'), true);
  assert.equal(quality('RedGreenHQCompression'), true);
  assert.equal(quality('ColorHQCompression'), true);
});

test('a quality is a fraction of one the recipe can write back exactly', () => {
  for (const accepted of [0, 1, 0.5, 0.403, 0.026, 0.03]) {
    assert.equal(isTextureQuality(accepted), true, String(accepted));
    assert.equal(Number(textureQualityText(accepted)), accepted);
  }
  for (const refused of [-0.001, 1.001, 0.4031, Number.NaN, Number.POSITIVE_INFINITY, '1', null]) {
    assert.equal(isTextureQuality(refused), false, String(refused));
  }
  assert.equal(textureQualityText(1), '1');
  assert.equal(textureQualityText(0.5), '0.5');
  assert.equal(textureQualityText(0.403), '0.403');
});

test('the channel view follows the channels the file holds, not the ones its source had', () => {
  const views = (channels: Parameters<typeof textureChannelViewsOf>[0]) =>
    textureChannelViewsOf(channels).map(({ view }) => view);
  assert.deepEqual(views('R'), ['rgba', 'red']);
  assert.deepEqual(views('RG'), ['rgba', 'red', 'green']);
  assert.deepEqual(views('RGB'), ['rgba', 'red', 'green', 'blue']);
  assert.deepEqual(views('RGBA'), ['rgba', 'red', 'green', 'blue', 'alpha']);
  assert.deepEqual(views('UNKNOWN'), ['rgba', 'red', 'green', 'blue', 'alpha']);
});
