import type { TextureProfile, TextureSourceFormat } from './textureConversion';

/**
 * The profile fields a Radiance source fixes, each with the reason the editors give: Workbench's
 * importer was captured filtering HDR in float, with no swizzle and no normalization.
 */
export const TEXTURE_HDR_FIXED_FIELDS = {
  Swizzling: 'HDR supports no swizzling.',
  Normalize: 'HDR preserves radiance without normalization.',
  MipMapFunction: 'HDR supports float Filter only.',
} as const;

/** Captured DayZ Workbench float-source contract, shared by plans, reducers and controls. */
export function textureHdrRefusalOf(
  profile: TextureProfile,
  sourceFormat: TextureSourceFormat | readonly TextureSourceFormat[],
  source?: { readonly width: number; readonly height: number },
): string | undefined {
  if (typeof sourceFormat !== 'string') {
    return sourceFormat.map((format) => textureHdrRefusalOf(profile, format, source))
      .find((reason) => reason !== undefined);
  }
  if (sourceFormat !== 'HDR') {
    return profile.GenerateCubemap || profile.Conversion === 'HDRCompression'
      ? 'HDRCompression and GenerateCubemap require a Radiance HDR source.' : undefined;
  }
  if (profile.Conversion !== 'None' && profile.Conversion !== 'HDRCompression') {
    return 'HDR supports Conversion=None or HDRCompression only.';
  }
  if (profile.Swizzling !== 'None') return TEXTURE_HDR_FIXED_FIELDS.Swizzling;
  if (profile.Normalize) return TEXTURE_HDR_FIXED_FIELDS.Normalize;
  if (profile.MipMapFunction !== 'Filter') return TEXTURE_HDR_FIXED_FIELDS.MipMapFunction;
  if (source !== undefined) {
    const powerOfTwo = (value: number) => value > 0 && (value & (value - 1)) === 0;
    if (profile.GenerateCubemap && (source.width !== source.height * 2 ||
      source.width < 16 || !powerOfTwo(source.width))) {
      return 'GenerateCubemap requires a -Y/+X 2:1 equirectangular HDR panorama, power-of-two width at least 16.';
    }
    if (profile.Conversion === 'HDRCompression' && (source.width < 4 || source.height < 4 ||
      !powerOfTwo(source.width) || !powerOfTwo(source.height))) {
      return 'HDRCompression requires power-of-two dimensions of at least 4.';
    }
  }
  return undefined;
}
