/**
 * The versioned values that cross the EDDS converter's process boundary.
 *
 * The native core is the only binary codec. This module only distrusts its JSON boundary: every
 * value is checked before the view can allocate from it or describe it as a fact of the file.
 */

export const EDDS_PROTOCOL_VERSION = 1;
export const EDDS_MAX_DIMENSION = 32_768;
export const EDDS_MAX_MIPS = 32;
export const EDDS_MAX_PREVIEW_BYTES = 64 * 1024 * 1024;

/** The two modern uncompressed formats the first preview slice can decode. */
export type PreviewablePixelFormat = 'BGRA8' | 'BGRX8';

/** Recognized formats stay printable even while this version cannot decode their pixels. */
export type EddsPixelFormat = PreviewablePixelFormat | 'DXT1' | 'DXT5' | `DXGI_${number}` | 'UNKNOWN';

/** The common DDS fields that remain useful even when the payload format is unsupported. */
export interface DdsFacts {
  readonly flags: number;
  readonly pitchOrLinearSize: number;
  readonly depth: number;
  readonly pixelFormatFlags: number;
  readonly fourCC: string;
  readonly rgbBitCount: number;
  readonly rMask: number;
  readonly gMask: number;
  readonly bMask: number;
  readonly aMask: number;
  readonly caps: number;
  readonly caps2: number;
  readonly dxgiFormat: number;
  readonly resourceDimension: number;
  readonly arraySize: number;
  readonly miscFlag: number;
}

/** One actual entry of the ENF1 mip table. Level zero is the largest image. */
export interface EddsMip {
  readonly level: number;
  readonly width: number;
  readonly height: number;
  readonly container: 'COPY' | 'LZ4';
  readonly storedBytes: number;
  readonly decodedBytes: number;
}

export interface EddsInspection {
  readonly width: number;
  readonly height: number;
  readonly pixelFormat: EddsPixelFormat;
  readonly dds: DdsFacts;
  readonly mips: readonly EddsMip[];
  readonly pixels:
    | { readonly kind: 'supported' }
    | { readonly kind: 'unsupported'; readonly reason: string };
}

/** Actual pixels decoded from one runtime mip, normalized to top-to-bottom RGBA. */
export interface EddsPreview {
  readonly level: number;
  readonly width: number;
  readonly height: number;
  readonly rgba: Uint8Array;
}

export interface EddsProtocol {
  readonly protocolVersion: typeof EDDS_PROTOCOL_VERSION;
  readonly toolVersion: string;
  readonly commands: readonly string[];
}

export type EddsFailureCategory =
  | 'invalid-invocation'
  | 'invalid-input'
  | 'unsupported-format'
  | 'cancelled'
  | 'internal-failure';

export interface EddsMachineFailure {
  readonly category: EddsFailureCategory;
  readonly code: string;
  readonly message: string;
}

/** The cached handshake, checked before an input path is handed to the converter. */
export function protocolOf(source: string): EddsProtocol {
  const value = envelopeOf(source, 'protocol');
  const toolVersion = stringOf(value, 'toolVersion');
  const commands = arrayOf(value, 'commands').map((command, at) =>
    stringValue(command, `commands[${at}]`),
  );

  if (!commands.includes('inspect')) {
    throw new Error('The EDDS converter protocol has no inspect command.');
  }
  if (!commands.includes('preview')) {
    throw new Error('The EDDS converter protocol has no preview command.');
  }

  return { protocolVersion: EDDS_PROTOCOL_VERSION, toolVersion, commands };
}

/** Common DDS and ENF1 facts from a successful inspect result. */
export function inspectionOf(source: string): EddsInspection {
  const value = envelopeOf(source, 'inspect');
  const width = positiveIntegerOf(value, 'width', EDDS_MAX_DIMENSION);
  const height = positiveIntegerOf(value, 'height', EDDS_MAX_DIMENSION);
  const mipCount = positiveIntegerOf(value, 'mipCount', EDDS_MAX_MIPS);
  const pixelFormat = pixelFormatOf(value.pixelFormat);
  const previewSupported = booleanOf(value, 'previewSupported');
  const ddsValue = objectValue(value.dds, 'dds');
  const dds: DdsFacts = {
    flags: unsignedOf(ddsValue, 'flags'),
    pitchOrLinearSize: unsignedOf(ddsValue, 'pitchOrLinearSize'),
    depth: unsignedOf(ddsValue, 'depth'),
    pixelFormatFlags: unsignedOf(ddsValue, 'pixelFormatFlags'),
    fourCC: stringOf(ddsValue, 'fourCC'),
    rgbBitCount: unsignedOf(ddsValue, 'rgbBitCount'),
    rMask: unsignedOf(ddsValue, 'rMask'),
    gMask: unsignedOf(ddsValue, 'gMask'),
    bMask: unsignedOf(ddsValue, 'bMask'),
    aMask: unsignedOf(ddsValue, 'aMask'),
    caps: unsignedOf(ddsValue, 'caps'),
    caps2: unsignedOf(ddsValue, 'caps2'),
    dxgiFormat: unsignedOf(ddsValue, 'dxgiFormat'),
    resourceDimension: unsignedOf(ddsValue, 'resourceDimension'),
    arraySize: unsignedOf(ddsValue, 'arraySize'),
    miscFlag: unsignedOf(ddsValue, 'miscFlag'),
  };
  const mips = arrayOf(value, 'mips').map(mipOf);

  if (mips.length !== mipCount) {
    throw new Error(`inspect.mipCount is ${mipCount}, but inspect.mips has ${mips.length} entries.`);
  }
  if (mips[0]?.width !== width || mips[0]?.height !== height) {
    throw new Error('inspect width and height do not match mip level 0.');
  }

  const canPreview = pixelFormat === 'BGRA8' || pixelFormat === 'BGRX8';
  if (previewSupported && !canPreview) {
    throw new Error(`This protocol cannot preview ${pixelFormat}, but the EDDS converter says it can.`);
  }
  const unsupportedReason = previewSupported
    ? undefined
    : optionalStringOf(value, 'previewUnsupportedReason') ??
      `Pixel preview is unavailable because ${pixelFormat} is not supported.`;

  return {
    width,
    height,
    pixelFormat,
    dds,
    mips,
    pixels: previewSupported
      ? { kind: 'supported' }
      : {
          kind: 'unsupported',
          reason:
            unsupportedReason ??
            `Pixel preview is unavailable because ${pixelFormat} is not supported.`,
        },
  };
}

/** One selected mip's actual runtime pixels from a successful preview result. */
export function previewOf(source: string): EddsPreview {
  const value = envelopeOf(source, 'preview');
  const level = nonnegativeIntegerOf(value, 'mip', EDDS_MAX_MIPS - 1);
  const width = positiveIntegerOf(value, 'width', EDDS_MAX_DIMENSION);
  const height = positiveIntegerOf(value, 'height', EDDS_MAX_DIMENSION);

  if (stringOf(value, 'pixelFormat') !== 'RGBA8') {
    throw new Error('preview.pixelFormat must be RGBA8.');
  }

  const byteLength = nonnegativeIntegerOf(value, 'byteLength', EDDS_MAX_PREVIEW_BYTES);
  const expected = width * height * 4;
  if (byteLength !== expected) {
    throw new Error(
      `preview.byteLength is ${byteLength}, but ${width}x${height} RGBA8 pixels require ${expected}.`,
    );
  }

  const encoded = stringOf(value, 'pixelsBase64');
  const rgba = base64Of(encoded, byteLength);
  return { level, width, height, rgba };
}

/** A structured failure is useful text; the process exit code remains its authoritative category. */
export function machineFailureOf(source: string): EddsMachineFailure {
  const value = envelopeOf(source, 'error');
  const error = objectValue(value.error, 'error');
  const category = stringOf(error, 'category');

  if (!isFailureCategory(category)) {
    throw new Error(`error.category is not recognized: ${category}.`);
  }

  return {
    category,
    code: stringOf(error, 'code'),
    message: stringOf(error, 'message'),
  };
}

function mipOf(value: unknown, at: number): EddsMip {
  const mip = objectValue(value, `mips[${at}]`);
  const level = nonnegativeIntegerOf(mip, 'level', EDDS_MAX_MIPS - 1);
  if (level !== at) {
    throw new Error(`mips[${at}].level must be ${at}, not ${level}.`);
  }

  const container = stringOf(mip, 'container');
  if (container !== 'COPY' && container !== 'LZ4') {
    throw new Error(`mips[${at}].container must be COPY or LZ4.`);
  }

  return {
    level,
    width: positiveIntegerOf(mip, 'width', EDDS_MAX_DIMENSION),
    height: positiveIntegerOf(mip, 'height', EDDS_MAX_DIMENSION),
    container,
    storedBytes: nonnegativeIntegerOf(mip, 'storedBytes', Number.MAX_SAFE_INTEGER),
    decodedBytes: positiveIntegerOf(mip, 'decodedBytes', Number.MAX_SAFE_INTEGER),
  };
}

function envelopeOf(source: string, kind: string): Record<string, unknown> {
  let parsed: unknown;
  try {
    parsed = JSON.parse(source);
  } catch {
    throw new Error('The EDDS converter did not return valid JSON.');
  }

  const value = objectValue(parsed, 'machine output');
  const version = integerValue(value.protocolVersion, 'protocolVersion');
  if (version !== EDDS_PROTOCOL_VERSION) {
    throw new Error(
      `The EDDS converter uses protocol version ${version}; this extension requires ${EDDS_PROTOCOL_VERSION}.`,
    );
  }
  if (stringOf(value, 'kind') !== kind) {
    throw new Error(`The EDDS converter did not return a ${kind} result.`);
  }

  return value;
}

function base64Of(encoded: string, byteLength: number): Uint8Array {
  const expectedCharacters = 4 * Math.ceil(byteLength / 3);
  const shape = /^(?:[A-Za-z0-9+/]{4})*(?:[A-Za-z0-9+/]{2}==|[A-Za-z0-9+/]{3}=)?$/;
  if (encoded.length !== expectedCharacters || !shape.test(encoded)) {
    throw new Error('preview.pixelsBase64 is not canonical base64 for the declared byteLength.');
  }

  const alphabet = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
  const decoded = new Uint8Array(byteLength);
  let written = 0;
  for (let at = 0; at < encoded.length; at += 4) {
    const a = alphabet.indexOf(encoded[at] ?? '=');
    const b = alphabet.indexOf(encoded[at + 1] ?? '=');
    const cText = encoded[at + 2] ?? '=';
    const dText = encoded[at + 3] ?? '=';
    const c = cText === '=' ? 0 : alphabet.indexOf(cText);
    const d = dText === '=' ? 0 : alphabet.indexOf(dText);
    const value = (a << 18) | (b << 12) | (c << 6) | d;

    if ((cText === '=' && (b & 0x0f) !== 0) || (dText === '=' && cText !== '=' && (c & 0x03) !== 0)) {
      throw new Error('preview.pixelsBase64 has non-canonical padding bits.');
    }
    if (written < decoded.length) decoded[written++] = value >> 16;
    if (cText !== '=' && written < decoded.length) decoded[written++] = value >> 8;
    if (dText !== '=' && written < decoded.length) decoded[written++] = value;
  }
  if (written !== byteLength) {
    throw new Error('preview.pixelsBase64 does not decode to the declared byteLength.');
  }

  return decoded;
}

function pixelFormatOf(value: unknown): EddsPixelFormat {
  const format = stringValue(value, 'pixelFormat');
  if (
    format === 'BGRA8' ||
    format === 'BGRX8' ||
    format === 'DXT1' ||
    format === 'DXT5' ||
    format === 'UNKNOWN' ||
    /^DXGI_(?:0|[1-9]\d*)$/.test(format)
  ) {
    return format as EddsPixelFormat;
  }

  throw new Error(`pixelFormat is not recognized: ${format}.`);
}

function objectValue(value: unknown, name: string): Record<string, unknown> {
  if (typeof value !== 'object' || value === null || Array.isArray(value)) {
    throw new Error(`${name} must be an object.`);
  }

  return value as Record<string, unknown>;
}

function arrayOf(value: Record<string, unknown>, name: string): readonly unknown[] {
  const found = value[name];
  if (!Array.isArray(found)) {
    throw new Error(`${name} must be an array.`);
  }

  return found;
}

function stringOf(value: Record<string, unknown>, name: string): string {
  return stringValue(value[name], name);
}

function optionalStringOf(value: Record<string, unknown>, name: string): string | undefined {
  const found = value[name];
  return found === undefined ? undefined : stringValue(found, name);
}

function stringValue(value: unknown, name: string): string {
  if (typeof value !== 'string' || value.length === 0) {
    throw new Error(`${name} must be a non-empty string.`);
  }

  return value;
}

function booleanOf(value: Record<string, unknown>, name: string): boolean {
  const found = value[name];
  if (typeof found !== 'boolean') {
    throw new Error(`${name} must be a boolean.`);
  }

  return found;
}

function unsignedOf(value: Record<string, unknown>, name: string): number {
  return nonnegativeIntegerOf(value, name, 0xffff_ffff);
}

function positiveIntegerOf(
  value: Record<string, unknown>,
  name: string,
  most: number,
): number {
  const found = integerValue(value[name], name);
  if (found < 1 || found > most) {
    throw new Error(`${name} must be a positive integer no greater than ${most}.`);
  }

  return found;
}

function nonnegativeIntegerOf(
  value: Record<string, unknown>,
  name: string,
  most: number,
): number {
  const found = integerValue(value[name], name);
  if (found < 0 || found > most) {
    throw new Error(`${name} must be an integer from 0 through ${most}.`);
  }

  return found;
}

function integerValue(value: unknown, name: string): number {
  if (typeof value !== 'number' || !Number.isSafeInteger(value)) {
    throw new Error(`${name} must be a safe integer.`);
  }

  return value;
}

function isFailureCategory(value: string): value is EddsFailureCategory {
  return (
    value === 'invalid-invocation' ||
    value === 'invalid-input' ||
    value === 'unsupported-format' ||
    value === 'cancelled' ||
    value === 'internal-failure'
  );
}
