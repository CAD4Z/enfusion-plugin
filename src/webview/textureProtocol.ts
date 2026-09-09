/** Messages crossing the read-only EDDS editor's host/browser boundary. */

import type { TextureChannel, TextureEditorState } from '../mods/textureEditor';

export interface TextureStateMessage {
  readonly type: 'state';
  readonly state: TextureEditorState;
}

export type TextureRequest =
  | { readonly type: 'ready' }
  | { readonly type: 'select-channel'; readonly channel: TextureChannel }
  | { readonly type: 'select-mip'; readonly mip: number };
