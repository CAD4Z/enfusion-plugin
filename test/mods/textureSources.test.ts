/**
 * The source contract is the one place the product decides what a source image is, so this is
 * where that decision is checked: the rows themselves, the Explorer menu that has to offer exactly
 * them, and the two aliases that look like supported formats and are not.
 */

import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { test } from 'node:test';
import {
  TEXTURE_PRIMARY_REFUSAL,
  TEXTURE_SOURCES,
  TEXTURE_SOURCE_EXTENSIONS,
  TEXTURE_SOURCE_EXTENSIONS_EITHER,
  TEXTURE_SOURCE_MENU_PATTERN,
  TEXTURE_SOURCE_REFUSAL,
  isTextureSourcePath,
  textureSourceFormatOf,
  textureSourceFormatOfWire,
} from '../../src/mods/textureSources';

test('the contract is the four resource classes DayZ Workbench registers, and nothing else', () => {
  assert.deepEqual(TEXTURE_SOURCES, [
    { format: 'PNG', extension: 'png', wire: 'png' },
    { format: 'TGA', extension: 'tga', wire: 'tga' },
    { format: 'JPG', extension: 'jpg', wire: 'jpg' },
    { format: 'TIFF', extension: 'tiff', wire: 'tiff' },
  ]);
});

test('an extension names its format however it was typed', () => {
  assert.equal(textureSourceFormatOf('C:\\mod\\Mod\\icon.JPG'), 'JPG');
  assert.equal(textureSourceFormatOf('C:\\mod\\Mod\\icon.TiFf'), 'TIFF');
  assert.equal(textureSourceFormatOf('C:\\mod\\Mod\\icon.png'), 'PNG');
  assert.equal(textureSourceFormatOf('C:\\mod\\Mod\\Icon.Tga'), 'TGA');
});

test('an alias is not a resource class, however much it reads like one', () => {
  for (const alias of ['C:\\mod\\Mod\\icon.jpeg', 'C:\\mod\\Mod\\icon.tif']) {
    assert.equal(textureSourceFormatOf(alias), undefined);
    assert.equal(isTextureSourcePath(alias), false);
  }
  assert.equal(textureSourceFormatOf('C:\\mod\\Mod\\icon.bmp'), undefined);
  assert.equal(textureSourceFormatOf('C:\\mod\\Mod\\icon'), undefined);
});

test('the native wire name maps back to the format a plan is written in', () => {
  assert.equal(textureSourceFormatOfWire('tiff'), 'TIFF');
  assert.equal(textureSourceFormatOfWire('jpg'), 'JPG');
  assert.equal(textureSourceFormatOfWire('jpeg'), undefined);
  assert.equal(textureSourceFormatOfWire('tif'), undefined);
});

test('refusals name the whole contract rather than a remembered pair', () => {
  assert.equal(TEXTURE_SOURCE_EXTENSIONS, '.png, .tga, .jpg and .tiff');
  assert.equal(TEXTURE_SOURCE_EXTENSIONS_EITHER, '.png, .tga, .jpg or .tiff');
  assert.equal(TEXTURE_SOURCE_REFUSAL, 'Only .png, .tga, .jpg and .tiff source images are supported.');
  assert.equal(TEXTURE_PRIMARY_REFUSAL, 'The primary source must be a PNG, TGA, JPG or TIFF image.');
});

/**
 * The menu is contributed by a `when` clause in the manifest, which no import can reach. If it
 * ever offers a format the handler refuses, or hides one it accepts, the two have drifted.
 */
test('the Explorer menu offers exactly the formats the handler accepts', () => {
  const manifest = JSON.parse(readFileSync(join(process.cwd(), 'package.json'), 'utf8')) as {
    contributes: { menus: Record<string, { command: string; when?: string }[]> };
  };
  const entry = Object.values(manifest.contributes.menus)
    .flat()
    .find(({ command }) => command === 'enfusion.texture.convert');

  assert.ok(entry?.when?.includes(TEXTURE_SOURCE_MENU_PATTERN), entry?.when);
});
