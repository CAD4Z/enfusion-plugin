/**
 * The one list of source images this product converts, and the only place any part of it decides
 * whether a path is one. The native converter carries the same table behind the same names, so a
 * format is added in two places and refused everywhere else by consequence rather than by memory.
 *
 * Each row is a texture resource class DayZ Workbench actually registers, under the single
 * extension that names it. Aliases are deliberately absent: Workbench registers `.jpg` and
 * `.tiff`, not `.jpeg` and `.tif`, so a file under an alias would otherwise convert into an EDDS
 * the editor calls registered and Workbench sees no source for.
 */

export interface TextureSource {
  /** How a plan, a metadata value and the editor name the format. */
  readonly format: TextureSourceFormat;
  /** The single extension, without its dot, matched without regard to case. */
  readonly extension: string;
  /** What the native machine protocol reports in `metadata.identity.sourceFormat`. */
  readonly wire: string;
}

export type TextureSourceFormat = 'PNG' | 'TGA' | 'JPG' | 'TIFF' | 'DDS';

/**
 * The Workbench resource class each row maps to is deliberately absent: only the native metadata
 * writer ever needs that name, and its own table owns it. Two copies of it could only drift.
 */
export const TEXTURE_SOURCES: readonly TextureSource[] = [
  { format: 'PNG', extension: 'png', wire: 'png' },
  { format: 'TGA', extension: 'tga', wire: 'tga' },
  { format: 'JPG', extension: 'jpg', wire: 'jpg' },
  { format: 'TIFF', extension: 'tiff', wire: 'tiff' },
  { format: 'DDS', extension: 'dds', wire: 'dds' },
];

/** The format of a path, or undefined when nothing in the contract claims that extension. */
export function textureSourceFormatOf(path: string): TextureSourceFormat | undefined {
  const extension = path.slice(path.lastIndexOf('.') + 1).toLowerCase();
  return TEXTURE_SOURCES.find((source) => source.extension === extension)?.format;
}

export function isTextureSourcePath(path: string): boolean {
  return textureSourceFormatOf(path) !== undefined;
}

/** The wire name the native protocol uses, mapped back to the format a plan is written in. */
export function textureSourceFormatOfWire(wire: string): TextureSourceFormat | undefined {
  return TEXTURE_SOURCES.find((source) => source.wire === wire)?.format;
}

/**
 * The `when` clause the Explorer menu is contributed under, so what the menu offers and what the
 * handler accepts cannot drift apart unnoticed.
 */
export const TEXTURE_SOURCE_MENU_PATTERN =
  `/\\.(${TEXTURE_SOURCES.map((source) => source.extension).join('|')})$/i`;

/** Every row spelled out in a sentence, so a refusal stays true as the contract grows. */
function listed(words: readonly string[], last: string): string {
  return words.length < 2
    ? words.join('')
    : `${words.slice(0, -1).join(', ')} ${last} ${words[words.length - 1]}`;
}

const extensions = TEXTURE_SOURCES.map((source) => `.${source.extension}`);

/** "all of these are supported" reads with `and`; "pick one of these" reads with `or`. */
export const TEXTURE_SOURCE_EXTENSIONS = listed(extensions, 'and');
export const TEXTURE_SOURCE_EXTENSIONS_EITHER = listed(extensions, 'or');

export const TEXTURE_SOURCE_REFUSAL = `Only ${TEXTURE_SOURCE_EXTENSIONS} source images are supported.`;

export const TEXTURE_PRIMARY_REFUSAL = `The primary source must be a ${listed(
  TEXTURE_SOURCES.map((source) => source.format),
  'or',
)} image.`;
