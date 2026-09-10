import type { TextureBatchAuthoringState } from '../mods/textureBatchAuthoring';
import type { TextureCompression } from '../mods/textureConversion';

export interface TextureBatchStateMessage {
  readonly type: 'state';
  readonly state: TextureBatchAuthoringState;
}

export type TextureBatchRequest =
  | { readonly type: 'ready' }
  | { readonly type: 'select-item'; readonly source: string }
  | { readonly type: 'change-compression'; readonly value: TextureCompression }
  | { readonly type: 'change-threshold'; readonly value: number }
  | { readonly type: 'change-mips'; readonly value: boolean }
  | { readonly type: 'run' }
  | { readonly type: 'cancel' }
  | { readonly type: 'retry-failed' };
