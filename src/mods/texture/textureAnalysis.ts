/** Read-only arithmetic over decoded samples. No codec, process, profile mutation or I/O. */
import type { EddsInspection, EddsPreview } from './edds';
import type { TextureProfile } from './textureConversion';
import type { TextureChannels, TextureChannelView } from './textureConversions';

/** A fixed work ceiling, independent of the converter's larger preview allocation limit. */
export const ANALYSIS_MAX_PIXELS = 1024 * 1024;
export const PREVIEW_CACHE_BYTES = 16 * 1024 * 1024;

export type AnalysisValue<T> =
  | { readonly kind: 'available'; readonly value: T }
  | { readonly kind: 'unavailable'; readonly reason: string };

/** Interleaved, straight RGBA in native sample units; floats retain their dynamic range. */
export interface AnalysisPixels {
  readonly level: number;
  readonly width: number;
  readonly height: number;
  readonly channels: TextureChannels | 'UNKNOWN';
  readonly rgba: Uint8Array | Float32Array;
}

type Channel = 'R' | 'G' | 'B' | 'A';
const COMPONENTS: readonly Channel[] = ['R', 'G', 'B', 'A'];
const VIEWS: readonly TextureChannelView[] = ['red', 'green', 'blue', 'alpha'];

export interface Histogram {
  readonly pixelCount: number;
  readonly minimum: number;
  readonly maximum: number;
  readonly series: readonly { readonly channel: Channel; readonly bins: readonly number[] }[];
}

function unavailable(reason: string): AnalysisValue<never> {
  return { kind: 'unavailable', reason };
}

function pixelRefusal(pixels: AnalysisPixels): string | undefined {
  const { width, height, rgba } = pixels;
  if (!Number.isSafeInteger(width) || !Number.isSafeInteger(height) || width < 1 || height < 1) {
    return 'Invalid decoded pixel dimensions.';
  }
  if (width * height > ANALYSIS_MAX_PIXELS) {
    return `Analysis is limited to ${ANALYSIS_MAX_PIXELS.toLocaleString('en-US')} pixels per mip. Select a smaller mip.`;
  }
  if (rgba.length !== width * height * 4) return 'Decoded RGBA samples do not match the mip dimensions.';
  if (pixels.channels === 'UNKNOWN') return 'The decoder has not identified the carried channels.';
  return undefined;
}

function componentsOf(pixels: AnalysisPixels, view: TextureChannelView): readonly number[] {
  return COMPONENTS.flatMap((name, at) =>
    pixels.channels.includes(name) && (view === 'rgba' || VIEWS[at] === view) ? [at] : []);
}

/** 256 exact byte bins, or 256 equal float intervals spanning at least [0, 1]. */
export function histogramOf(pixels: AnalysisPixels, view: TextureChannelView): AnalysisValue<Histogram> {
  const refusal = pixelRefusal(pixels);
  if (refusal !== undefined) return unavailable(refusal);
  const components = componentsOf(pixels, view);
  if (components.length === 0) return unavailable('The selected channel is not carried by these pixels.');
  let minimum = 0;
  let maximum = pixels.rgba instanceof Float32Array ? 1 : 255;
  if (pixels.rgba instanceof Float32Array) {
    for (let at = 0; at < pixels.rgba.length; at += 4) {
      for (const component of components) {
        const value = pixels.rgba[at + component];
        if (!Number.isFinite(value)) return unavailable('Decoded pixels contain non-finite samples.');
        minimum = Math.min(minimum, value);
        maximum = Math.max(maximum, value);
      }
    }
  }
  const series = components.map((component) => ({
    channel: COMPONENTS[component], bins: new Array<number>(256).fill(0),
  }));
  for (let at = 0; at < pixels.rgba.length; at += 4) {
    for (let index = 0; index < components.length; index++) {
      const value = pixels.rgba[at + components[index]];
      const bin = pixels.rgba instanceof Float32Array
        ? Math.min(255, Math.floor((value - minimum) * 256 / (maximum - minimum))) : value;
      series[index].bins[bin]++;
    }
  }
  return { kind: 'available', value: { pixelCount: pixels.width * pixels.height, minimum, maximum, series } };
}

export interface PixelSample {
  readonly x: number;
  readonly y: number;
  readonly level: number;
  readonly channels: TextureChannels | 'UNKNOWN';
  readonly values: readonly number[];
}

/** The live canvas bounding rectangle includes CSS zoom, scrolling, padding and pan. */
export function pixelAt(
  pixels: AnalysisPixels,
  pointer: { readonly x: number; readonly y: number },
  rect: { readonly left: number; readonly top: number; readonly width: number; readonly height: number },
): AnalysisValue<PixelSample> {
  const refusal = pixelRefusal(pixels);
  if (refusal !== undefined) return unavailable(refusal);
  if (![pointer.x, pointer.y, rect.left, rect.top, rect.width, rect.height].every(Number.isFinite) ||
      rect.width <= 0 || rect.height <= 0) return unavailable('The viewport has no measurable pixel position.');
  const x = Math.floor((pointer.x - rect.left) * pixels.width / rect.width);
  const y = Math.floor((pointer.y - rect.top) * pixels.height / rect.height);
  if (x < 0 || y < 0 || x >= pixels.width || y >= pixels.height) return unavailable('Point inside the image to inspect a pixel.');
  const at = (y * pixels.width + x) * 4;
  const values = componentsOf(pixels, 'rgba').map((component) => pixels.rgba[at + component]);
  if (!values.every(Number.isFinite)) return unavailable('Decoded pixel contains non-finite samples.');
  return { kind: 'available', value: { x, y, level: pixels.level, channels: pixels.channels, values } };
}

export interface ErrorMetric {
  readonly rmse: number;
  readonly samples: number;
  readonly units: string;
}

/** sqrt(sum((reference - result)^2) / samples), row-major, without gamma or alpha weighting. */
export function errorMetricOf(
  reference: AnalysisPixels, result: AnalysisPixels, view: TextureChannelView,
): AnalysisValue<ErrorMetric> {
  const refusal = pixelRefusal(reference) ?? pixelRefusal(result);
  if (refusal !== undefined) return unavailable(refusal);
  if (reference.level !== result.level || reference.width !== result.width || reference.height !== result.height) {
    return unavailable('Source and Result must describe the same mip and dimensions.');
  }
  const floating = result.rgba instanceof Float32Array;
  if (floating !== (reference.rgba instanceof Float32Array)) return unavailable('Source and Result use different sample units.');
  const components = componentsOf(result, view);
  if (components.length === 0 || components.some((at) => !reference.channels.includes(COMPONENTS[at]))) {
    return unavailable('The selected channels are not carried by both Source and Result.');
  }
  let squared = 0;
  for (let at = 0; at < result.rgba.length; at += 4) {
    for (const component of components) {
      const difference = reference.rgba[at + component] - result.rgba[at + component];
      if (!Number.isFinite(difference)) return unavailable('Decoded pixels contain non-finite samples.');
      squared += difference * difference;
    }
  }
  const samples = result.width * result.height * components.length;
  return { kind: 'available', value: {
    rmse: Math.sqrt(squared / samples), samples, units: floating ? 'linear float values' : 'byte values (0–255)',
  } };
}

export interface RuntimeMemory {
  readonly mipBytes: number;
  readonly chainBytes: number;
}

/** Native inspection's decodedBytes is the GPU payload after container decompression, not RGBA preview bytes. */
export function runtimeMemoryOf(inspection: EddsInspection, level: number): AnalysisValue<RuntimeMemory> {
  if (inspection.channels === 'UNKNOWN') return unavailable('Runtime storage is unknown for this pixel format.');
  const dds = inspection.dds;
  if (dds.depth > 1 || dds.arraySize > 1 || (dds.caps2 & 0x200) !== 0 || (dds.miscFlag & 4) !== 0) {
    return unavailable('Runtime size requires decoded payload sizes for every texture surface.');
  }
  const mipBytes = inspection.mips.find((mip) => mip.level === level)?.decodedBytes;
  const chainBytes = inspection.mips.reduce((sum, mip) => sum + mip.decodedBytes, 0);
  if (mipBytes === undefined || !Number.isSafeInteger(chainBytes) || chainBytes <= 0 ||
      inspection.mips.some((mip) => !Number.isSafeInteger(mip.decodedBytes) || mip.decodedBytes <= 0)) {
    return unavailable('The actual mip payload sizes are unavailable.');
  }
  return { kind: 'available', value: { mipBytes, chainBytes } };
}

export interface TextureAnalysisInput {
  readonly inspection: EddsInspection;
  readonly source?: EddsPreview;
  readonly result?: EddsPreview;
  readonly sourceFacts?: { readonly hasAlpha: boolean };
  readonly profile?: TextureProfile;
  readonly reason?: string;
  readonly selectedMip?: number;
}

export interface AnalysisSelection {
  readonly side: 'source' | 'result';
  readonly channel: TextureChannelView;
}

export interface TextureAnalysis {
  readonly pixels: AnalysisValue<AnalysisPixels>;
  readonly histogram: AnalysisValue<Histogram>;
  readonly memory: AnalysisValue<RuntimeMemory>;
  readonly error: AnalysisValue<ErrorMetric>;
}

/** Existing source previews are original samples: only an unchanged base pipeline is comparable. */
function referenceOf(input: TextureAnalysisInput): AnalysisValue<AnalysisPixels> {
  if (input.source === undefined) return unavailable('Standalone EDDS has no Source pipeline pixels to compare.');
  const profile = input.profile;
  if (profile === undefined) return unavailable('The Source pipeline profile is unavailable.');
  if (profile.Swizzling !== 'None' || profile.Normalize || profile.RemoveMips !== 0) {
    return unavailable('The Source preview precedes swizzling, normalization and mip removal; matching pipeline samples are unavailable.');
  }
  if (input.result?.level !== 0) return unavailable('Source preview contains mip 0 only; matching Source pipeline mip samples are unavailable.');
  return { kind: 'available', value: { ...input.source, channels: input.sourceFacts?.hasAlpha === false ? 'RGB' : 'RGBA' } };
}

/** One pure projection shared by standalone, single-source and batch viewports. */
export function analysisOf(input: TextureAnalysisInput, selection: AnalysisSelection): TextureAnalysis {
  const preview = selection.side === 'source' ? input.source : input.result;
  const reason = input.reason ?? (input.inspection.pixels.kind === 'unsupported'
    ? input.inspection.pixels.reason : 'Decoded pixels are not available yet.');
  const pixels: AnalysisValue<AnalysisPixels> = preview === undefined ? unavailable(reason) : {
    kind: 'available', value: {
      ...preview, channels: selection.side === 'result' ? input.inspection.channels
        : input.sourceFacts?.hasAlpha === false ? 'RGB' : 'RGBA',
    },
  };
  const reference = referenceOf(input);
  const error = reference.kind === 'unavailable' ? reference : input.result === undefined ? unavailable(reason)
    : errorMetricOf(reference.value, { ...input.result, channels: input.inspection.channels }, selection.channel);
  return {
    pixels,
    histogram: pixels.kind === 'available' ? histogramOf(pixels.value, selection.channel) : pixels,
    memory: runtimeMemoryOf(input.inspection, input.result?.level ?? input.selectedMip ?? 0),
    error,
  };
}

/** Newest first, sharing sample arrays. An oversized preview is shown but never retained in the cache. */
export function cachePreview<T extends { readonly result: EddsPreview; readonly source?: EddsPreview }>(
  previous: readonly T[], next: T,
): readonly T[] {
  const candidates = [next, ...previous.filter((item) => item.result.level !== next.result.level)];
  const retained: T[] = [];
  let bytes = 0;
  for (const item of candidates) {
    bytes += item.result.rgba.byteLength + (item.source?.rgba.byteLength ?? 0);
    if (bytes > PREVIEW_CACHE_BYTES) break;
    retained.push(item);
  }
  return retained;
}
