import type { TextureAuthoringState } from '../mods/textureAuthoring';
import type {
  TextureCompression,
  TextureMipFilter,
  TextureMipFunction,
} from '../mods/textureConversion';

export interface TextureAuthoringStateMessage {
  readonly type: 'state';
  readonly state: TextureAuthoringState;
}

export type TextureAuthoringRequest =
  | { readonly type: 'ready' }
  | { readonly type: 'change-compression'; readonly value: TextureCompression }
  | { readonly type: 'change-threshold'; readonly value: number }
  | { readonly type: 'change-remove-mips'; readonly value: number }
  | { readonly type: 'change-contains-mips'; readonly value: boolean }
  | { readonly type: 'change-mips'; readonly value: boolean }
  | { readonly type: 'change-tiled-texture'; readonly value: boolean }
  | { readonly type: 'change-normalize'; readonly value: boolean }
  | { readonly type: 'change-mipmap-function'; readonly value: TextureMipFunction }
  | { readonly type: 'change-mipmap-filter'; readonly value: TextureMipFilter }
  | { readonly type: 'change-swizzling'; readonly value: string }
  | { readonly type: 'change-conversion'; readonly value: string }
  | { readonly type: 'change-quality'; readonly value: number }
  | { readonly type: 'select-mip'; readonly mip: number }
  | { readonly type: 'run' };
