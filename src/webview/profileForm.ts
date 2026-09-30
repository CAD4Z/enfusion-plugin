/**
 * The one texture-recipe form. Both editors show the same profile, so they show it through the
 * same controls: a field that is not applicable to the draft is rendered disabled with the reason
 * it is not, rather than left live and quietly ignored.
 */

import { TEXTURE_SWIZZLES, isTextureSwizzling, textureSwizzleRefusalOf, type TextureSwizzleSource } from '../mods/textureSwizzles';
import { textureProfileFieldsOf } from '../mods/textureAuthoring';
import type {
  TextureCompression,
  TextureMipFilter,
  TextureMipFunction,
  TextureProfile,
  TextureSourceFormat,
} from '../mods/textureConversion';
import {
  SUPPORTED_TEXTURE_CONVERSIONS,
  isSupportedTextureConversion,
  isTextureQuality,
} from '../mods/textureConversions';

export interface ProfileFormRequests {
  readonly compression: (value: TextureCompression) => void;
  readonly threshold: (value: number) => void;
  readonly removeMips: (value: number) => void;
  readonly containsMips: (value: boolean) => void;
  readonly mips: (value: boolean) => void;
  readonly tiled: (value: boolean) => void;
  readonly normalize: (value: boolean) => void;
  readonly mipFunction: (value: TextureMipFunction) => void;
  readonly mipFilter: (value: TextureMipFilter) => void;
  readonly swizzling: (value: string) => void;
  readonly conversion: (value: string) => void;
  readonly quality: (value: number) => void;
}

const COMPRESSIONS: readonly TextureCompression[] = ['Copy', 'Fastest', 'Medium', 'Best'];
const MIP_FUNCTIONS: readonly TextureMipFunction[] = ['Filter', 'Normalize', 'ColorNoise'];
const MIP_FILTERS: readonly TextureMipFilter[] = ['Box', 'Kaiser'];

export function profileFormControls(
  profile: TextureProfile,
  sourceFormat: TextureSourceFormat,
  locked: boolean,
  requests: ProfileFormRequests,
  source?: TextureSwizzleSource,
): readonly HTMLLabelElement[] {
  return textureProfileFieldsOf(profile, sourceFormat, source).map((field) => {
    const control = controlOf(profile, field.key, requests, source);
    control.disabled = locked || !field.editable;
    control.title = locked
      ? `${field.key}: properties are locked while or after this immutable run.`
      : `${field.key}${field.reason === undefined ? '' : `: ${field.reason}`}`;
    const label = document.createElement('label');
    label.className = 'control';
    const caption = document.createElement('span');
    caption.textContent = field.key;
    label.append(caption, control);
    return label;
  });
}

function controlOf(
  profile: TextureProfile,
  key: keyof TextureProfile,
  requests: ProfileFormRequests,
  source?: TextureSwizzleSource,
): HTMLInputElement | HTMLSelectElement {
  if (key === 'FormatCompress') {
    return selectOf(COMPRESSIONS, profile.FormatCompress, (value) => {
      if (COMPRESSIONS.includes(value as TextureCompression)) {
        requests.compression(value as TextureCompression);
      }
    });
  }
  if (key === 'Swizzling') {
    return selectOf(TEXTURE_SWIZZLES.map(({ name }) => name), profile.Swizzling, (value) => {
      if (isTextureSwizzling(value)) requests.swizzling(value);
    }, undefined, TEXTURE_SWIZZLES.map(({ name }) => textureSwizzleRefusalOf({ ...profile, Swizzling: name }, source)));
  }
  if (key === 'Conversion') {
    return selectOf(
      SUPPORTED_TEXTURE_CONVERSIONS.map((conversion) => conversion.name),
      profile.Conversion,
      (value) => {
        if (isSupportedTextureConversion(value)) {
          requests.conversion(value);
        }
      },
      SUPPORTED_TEXTURE_CONVERSIONS.map((conversion) => conversion.label),
      SUPPORTED_TEXTURE_CONVERSIONS.map(({ name }) => textureSwizzleRefusalOf({ ...profile, Conversion: name }, source)),
    );
  }
  if (key === 'MipMapFunction') {
    return selectOf(MIP_FUNCTIONS, profile.MipMapFunction, (value) => {
      if (MIP_FUNCTIONS.includes(value as TextureMipFunction)) {
        requests.mipFunction(value as TextureMipFunction);
      }
    });
  }
  if (key === 'MipMapFilter') {
    return selectOf(MIP_FILTERS, profile.MipMapFilter, (value) => {
      if (MIP_FILTERS.includes(value as TextureMipFilter)) {
        requests.mipFilter(value as TextureMipFilter);
      }
    });
  }
  const input = document.createElement('input');
  if (
    key === 'ContainsMips' || key === 'GenerateMips' || key === 'Normalize' ||
    key === 'TiledTexture'
  ) {
    input.type = 'checkbox';
    input.checked = Boolean(profile[key]);
    if (key === 'ContainsMips') input.addEventListener('change', () => requests.containsMips(input.checked));
    if (key === 'GenerateMips') input.addEventListener('change', () => requests.mips(input.checked));
    if (key === 'TiledTexture') input.addEventListener('change', () => requests.tiled(input.checked));
    if (key === 'Normalize') input.addEventListener('change', () => requests.normalize(input.checked));
    return input;
  }
  if (key === 'CompressTreshold') {
    input.type = 'number';
    input.min = '0';
    input.max = '100';
    input.step = '1';
    input.value = String(profile.CompressTreshold);
    input.addEventListener('change', () => requests.threshold(Number(input.value)));
    return input;
  }
  if (key === 'RemoveMips') {
    input.type = 'number';
    input.min = '0';
    input.max = '14';
    input.step = '1';
    input.value = String(profile.RemoveMips);
    input.addEventListener('change', () => requests.removeMips(Number(input.value)));
    return input;
  }
  if (key === 'ConversionQuality') {
    /* A fraction of one, to three decimals, which is the precision a recipe can write back. */
    input.type = 'number';
    input.min = '0';
    input.max = '1';
    input.step = '0.001';
    input.value = String(profile.ConversionQuality);
    input.addEventListener('change', () => {
      const value = Number(input.value);
      if (isTextureQuality(value)) {
        requests.quality(value);
      } else {
        input.value = String(profile.ConversionQuality);
      }
    });
    return input;
  }
  input.type = 'text';
  input.value = String(profile[key]);
  return input;
}

function selectOf(
  values: readonly string[],
  selected: string,
  onChange: (value: string) => void,
  labels: readonly string[] = values,
  refusals: readonly (string | undefined)[] = [],
): HTMLSelectElement {
  const select = document.createElement('select');
  values.forEach((value, at) => {
    const option = document.createElement('option');
    option.value = value;
    option.textContent = labels[at] ?? value;
    option.selected = value === selected;
    option.disabled = refusals[at] !== undefined;
    option.title = refusals[at] ?? '';
    if (option.disabled) option.textContent += ` - ${refusals[at]}`;
    select.append(option);
  });
  select.addEventListener('change', () => onChange(select.value));
  return select;
}
