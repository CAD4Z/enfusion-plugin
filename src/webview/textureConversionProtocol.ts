import type { TextureAuthoringState } from '../mods/textureAuthoring';
import type { TextureCompression } from '../mods/textureConversion';

export interface TextureAuthoringStateMessage {
  readonly type: 'state';
  readonly state: TextureAuthoringState;
}

export type TextureAuthoringRequest =
  | { readonly type: 'ready' }
  | { readonly type: 'change-compression'; readonly value: TextureCompression }
  | { readonly type: 'change-threshold'; readonly value: number }
  | { readonly type: 'change-mips'; readonly value: boolean }
  | { readonly type: 'change-conversion'; readonly value: string }
  | { readonly type: 'change-quality'; readonly value: number }
  | { readonly type: 'select-mip'; readonly mip: number }
  | { readonly type: 'run' };
