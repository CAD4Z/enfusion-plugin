import type { TextureRendering } from '../../mods/texture/textureAuthoring';
import type { TextureBatchAuthoringState } from '../../mods/texture/textureBatchAuthoring';
import type {
  TextureCompression,
  TextureMipFilter,
  TextureMipFunction,
} from '../../mods/texture/textureConversion';

/**
 * The editor's state with the pixels of its rendering left empty: a batch posts a state for every
 * progress step, and the pixels — up to 64 MiB a side — cross once, in `rendering`, when they are
 * a different rendering from the one the panel already shows. `null` takes the shown one away.
 */
export interface TextureBatchStateMessage {
  readonly type: 'state';
  readonly state: TextureBatchAuthoringState;
  readonly rendering?: TextureRendering | null;
}

export type TextureBatchRequest =
  | { readonly type: 'ready' }
  | { readonly type: 'select-item'; readonly source: string }
  | { readonly type: 'change-compression'; readonly value: TextureCompression }
  | { readonly type: 'change-threshold'; readonly value: number }
  | { readonly type: 'change-remove-mips'; readonly value: number }
  | { readonly type: 'change-contains-mips'; readonly value: boolean }
  | { readonly type: 'change-cubemap'; readonly value: boolean }
  | { readonly type: 'change-mips'; readonly value: boolean }
  | { readonly type: 'change-tiled-texture'; readonly value: boolean }
  | { readonly type: 'change-normalize'; readonly value: boolean }
  | { readonly type: 'change-mipmap-function'; readonly value: TextureMipFunction }
  | { readonly type: 'change-mipmap-filter'; readonly value: TextureMipFilter }
  | { readonly type: 'change-swizzling'; readonly value: string }
  | { readonly type: 'change-conversion'; readonly value: string }
  | { readonly type: 'change-quality'; readonly value: number }
  | { readonly type: 'run' }
  | { readonly type: 'cancel' }
  | { readonly type: 'retry-failed' };
