/**
 * The one list of Workbench `Conversion` values this product understands, and the only place any
 * part of it decides what a conversion produces. The native converter carries the same table
 * behind the same names, so a conversion is added in two places and refused everywhere else by
 * consequence rather than by memory.
 *
 * Every row names the runtime format the result is stored in and the channels a decode of it
 * really carries — which is a fact about the file, not about the image it came from: a photograph
 * converted through `Red` is one channel afterwards, whatever it had before.
 */

export type TextureConversion =
  | 'None'
  | 'DXTCompression'
  | 'Red'
  | 'RedHQCompression'
  | 'RedGreen'
  | 'RedGreenHQCompression'
  | 'ColorHQCompression'
  | 'HDRCompression';

/** The runtime formats this converter can read back, which is every one it can also write. */
export type DecodablePixelFormat =
  | 'BGRA8'
  | 'BGRX8'
  | 'R8'
  | 'RG8'
  | 'DXT1'
  | 'DXT5'
  | 'BC4'
  | 'BC5'
  | 'BC7';

export type TextureChannels = 'R' | 'RG' | 'RGB' | 'RGBA';

export interface TextureConversionCapability {
  /** The Workbench enum, which is what a recipe on disk holds. */
  readonly name: TextureConversion;
  /** What the native CLI takes on its `--conversion` flag. */
  readonly wire: string;
  /** What the editor shows for it. */
  readonly label: string;
  /**
   * The runtime formats it can produce. `None` and `DXTCompression` have two: which one a source
   * lands on is read off that source's own alpha, never off the batch it was selected with.
   */
  readonly formats: readonly DecodablePixelFormat[];
  readonly channels: TextureChannels;
  /**
   * Whether `ConversionQuality` reaches the encoder at all. Workbench describes the field as
   * "Conversion quality for compressed formats", and an uncompressed conversion stores the source
   * channels as they are, so there is nothing for a quality to trade against.
   */
  readonly usesQuality: boolean;
  /** Whether this slice implements it; a recognized conversion it does not is refused, not mapped. */
  readonly supported: boolean;
}

export const TEXTURE_CONVERSIONS: readonly TextureConversionCapability[] = [
  {
    name: 'None',
    wire: 'none',
    label: 'None (uncompressed colour)',
    formats: ['BGRX8', 'BGRA8'],
    channels: 'RGBA',
    usesQuality: false,
    supported: true,
  },
  {
    name: 'DXTCompression',
    wire: 'dxt-compression',
    label: 'DXT (BC1 or BC3 by alpha)',
    formats: ['DXT1', 'DXT5'],
    channels: 'RGBA',
    usesQuality: true,
    supported: true,
  },
  {
    name: 'Red',
    wire: 'red',
    label: 'Red (uncompressed single channel)',
    formats: ['R8'],
    channels: 'R',
    usesQuality: false,
    supported: true,
  },
  {
    name: 'RedHQCompression',
    wire: 'red-hq-compression',
    label: 'Red HQ (BC4)',
    formats: ['BC4'],
    channels: 'R',
    usesQuality: true,
    supported: true,
  },
  {
    name: 'RedGreen',
    wire: 'red-green',
    label: 'Red/Green (uncompressed two channels)',
    formats: ['RG8'],
    channels: 'RG',
    usesQuality: false,
    supported: true,
  },
  {
    name: 'RedGreenHQCompression',
    wire: 'red-green-hq-compression',
    label: 'Red/Green HQ (BC5)',
    formats: ['BC5'],
    channels: 'RG',
    usesQuality: true,
    supported: true,
  },
  {
    name: 'ColorHQCompression',
    wire: 'color-hq-compression',
    label: 'Colour HQ (BC7)',
    formats: ['BC7'],
    channels: 'RGBA',
    usesQuality: true,
    supported: true,
  },
  {
    name: 'HDRCompression',
    wire: 'hdr-compression',
    label: 'HDR (BC6H)',
    formats: [],
    channels: 'RGB',
    usesQuality: true,
    supported: false,
  },
];

/** The conversions a profile may actually be set to. */
export const SUPPORTED_TEXTURE_CONVERSIONS: readonly TextureConversionCapability[] =
  TEXTURE_CONVERSIONS.filter((conversion) => conversion.supported);

export function textureConversionCapabilityOf(
  name: TextureConversion,
): TextureConversionCapability | undefined {
  return TEXTURE_CONVERSIONS.find((conversion) => conversion.name === name);
}

export function isSupportedTextureConversion(value: string): value is TextureConversion {
  return SUPPORTED_TEXTURE_CONVERSIONS.some((conversion) => conversion.name === value);
}

/**
 * `ConversionQuality` is a fraction of one, and DayZ's own recipes write it to three decimals.
 * A value with more precision than that could not be written back as the same number, so it is not
 * a quality this converter accepts rather than one it quietly rounds.
 */
export function isTextureQuality(value: unknown): value is number {
  if (typeof value !== 'number' || !Number.isFinite(value) || value < 0 || value > 1) {
    return false;
  }
  const thousandths = value * 1000;
  return Math.abs(thousandths - Math.round(thousandths)) < 1e-9;
}

/** The shortest text that reads back as exactly this quality: `1`, `0.5`, `0.403`. */
export function textureQualityText(value: number): string {
  return String(Math.round(value * 1000) / 1000);
}

/** The channel views of a format whose decode actually carries something to show. */
export type TextureChannelView = 'rgba' | 'red' | 'green' | 'blue' | 'alpha';

const CHANNEL_VIEWS: readonly { readonly view: TextureChannelView; readonly label: string }[] = [
  { view: 'rgba', label: 'RGBA' },
  { view: 'red', label: 'R' },
  { view: 'green', label: 'G' },
  { view: 'blue', label: 'B' },
  { view: 'alpha', label: 'A' },
];

/**
 * A single-channel texture has no green to look at, so the editor does not offer one: a button
 * that always showed black would read as a channel that had been lost rather than one the format
 * never had. What is offered follows the file, not the image it was made from.
 */
export function textureChannelViewsOf(
  channels: TextureChannels | 'UNKNOWN',
): readonly { readonly view: TextureChannelView; readonly label: string }[] {
  const carried = channels === 'UNKNOWN' ? 'RGBA' : channels;
  return CHANNEL_VIEWS.filter(({ view }) =>
    view === 'rgba' ||
    (view === 'red' && carried.includes('R')) ||
    (view === 'green' && carried.includes('G')) ||
    (view === 'blue' && carried.includes('B')) ||
    (view === 'alpha' && carried === 'RGBA'),
  );
}
