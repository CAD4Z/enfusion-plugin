import type { TextureProfile } from './textureConversion';

/** Mirrors native/src/swizzling.c; names are preserved verbatim in canonical recipes. */
export const TEXTURE_SWIZZLES = [
  { name: 'None', wire: 'none' },
  { name: 'TerrainLayerTexture', wire: 'terrain-layer-texture' },
  { name: 'TerrainSuperTexture', wire: 'terrain-super-texture' },
  { name: 'TerrainNormalSpecular_SYxX', wire: 'terrain-normal-specular-syxx' },
  { name: 'AlphaToRGB', wire: 'alpha-to-rgb' },
  { name: 'SMDIToGS', wire: 'smdi-to-gs' },
  { name: 'NormalMap_NOHQ', wire: 'normal-map-nohq' },
  { name: 'NormalMapGA', wire: 'normal-map-ga' },
  { name: 'NormalSpecularMapXYZS', wire: 'normal-specular-map-xyzs' },
  { name: 'AmbientSpecularMapGA', wire: 'ambient-specular-map-ga' },
] as const;

export type TextureSwizzling = (typeof TEXTURE_SWIZZLES)[number]['name'];

export interface TextureSwizzleSource {
  readonly width: number;
  readonly height: number;
  readonly hasAlpha: boolean;
}

export function isTextureSwizzling(value: unknown): value is TextureSwizzling {
  return TEXTURE_SWIZZLES.some(({ name }) => name === value);
}

export function textureSwizzleWireOf(value: TextureSwizzling): string {
  const capability = TEXTURE_SWIZZLES.find(({ name }) => name === value);
  if (capability === undefined) throw new Error(`Unsupported Swizzling: ${value}.`);
  return capability.wire;
}

export function textureSwizzleRefusalOf(profile: TextureProfile, source?: TextureSwizzleSource): string | undefined {
  if (!isTextureSwizzling(profile.Swizzling)) return 'Swizzling is unknown or unsupported.';
  const terrain = (profile.Swizzling === 'TerrainLayerTexture' || profile.Swizzling === 'TerrainSuperTexture') &&
    (profile.Conversion === 'None' || profile.Conversion === 'DXTCompression');
  const ambient = profile.Swizzling === 'AmbientSpecularMapGA' && profile.Conversion !== 'Red';
  if (terrain && (!profile.GenerateMips || profile.ContainsMips)) {
    return 'Terrain layer/super swizzling requires generated mips; supplied or disabled mips are incompatible.';
  }
  if (ambient && profile.RemoveMips > 0 && profile.ContainsMips) {
    return 'AmbientSpecularMapGA with RemoveMips requires an unsupplied source.';
  }
  if (source !== undefined && profile.RemoveMips > 0 && (terrain || ambient)) {
    if (source.width < 8 || source.height < 8) return 'Terrain/ambient RemoveMips requires a source of at least 8x8.';
    if (ambient && !source.hasAlpha) return 'AmbientSpecularMapGA with RemoveMips requires a source alpha channel.';
  }
  return undefined;
}
