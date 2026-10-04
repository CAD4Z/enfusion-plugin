/**
 * The state of the EDDS preview, independent of both VS Code and the browser rendering it.
 *
 * The preview opens an EDDS directly and only reads it; what it may offer beyond that is a
 * reconversion, when the texture's metadata names a source image the conversion editor can open.
 * Native work is returned as an effect; channel changes consume the pixels already present and
 * return no work.
 */

import type { EddsInspection, EddsPreview } from './edds';

export type TextureChannel = 'rgba' | 'red' | 'green' | 'blue' | 'alpha';

export const RECONVERT_REFUSAL =
  'Reconvert is unavailable because this EDDS has no validated texture metadata relation to a ' +
  'source image. A source is never inferred from a sibling with the same name.';

interface ReadOnlyTexture {
  readonly readOnly: true;
  readonly reconvert:
    | { readonly kind: 'refused'; readonly reason: string; readonly source?: undefined }
    | { readonly kind: 'available'; readonly source: string; readonly reason?: undefined };
}

export interface LoadingTexture extends ReadOnlyTexture {
  readonly kind: 'loading';
  readonly channel: TextureChannel;
}

export type TexturePreview =
  | { readonly kind: 'loading'; readonly mip: number; readonly request: number }
  | { readonly kind: 'ready'; readonly preview: EddsPreview }
  | { readonly kind: 'unsupported-format'; readonly reason: string }
  | { readonly kind: 'failed'; readonly reason: string };

export interface InspectedTexture extends ReadOnlyTexture {
  readonly kind: 'inspect-only';
  readonly channel: TextureChannel;
  readonly selectedMip: number;
  readonly inspection: EddsInspection;
  readonly preview: TexturePreview;
  /** The id the next decode receives, so a late response can be recognized without a clock. */
  readonly nextRequest: number;
}

export interface FailedTexture extends ReadOnlyTexture {
  readonly kind: 'failed';
  readonly channel: TextureChannel;
  readonly reason: string;
}

export type TextureEditorState = LoadingTexture | InspectedTexture | FailedTexture;

export type TextureEditorEvent =
  | { readonly kind: 'inspected'; readonly inspection: EddsInspection }
  | { readonly kind: 'inspection-failed'; readonly reason: string }
  | { readonly kind: 'select-channel'; readonly channel: TextureChannel }
  | { readonly kind: 'select-mip'; readonly mip: number }
  | { readonly kind: 'previewed'; readonly request: number; readonly preview: EddsPreview }
  | { readonly kind: 'preview-failed'; readonly request: number; readonly reason: string }
  | { readonly kind: 'reconversion-available'; readonly source: string }
  | { readonly kind: 'reconversion-refused'; readonly reason: string };

export type TextureEditorEffect =
  | { readonly kind: 'inspect' }
  | { readonly kind: 'preview'; readonly mip: number; readonly request: number };

export interface TextureEditorUpdate {
  readonly state: TextureEditorState;
  readonly effects: readonly TextureEditorEffect[];
}

const READ_ONLY: ReadOnlyTexture = {
  readOnly: true,
  reconvert: { kind: 'refused', reason: RECONVERT_REFUSAL },
};

/** A directly opened EDDS starts with one job: inspect the runtime texture itself. */
export function openedTexture(): TextureEditorUpdate {
  return {
    state: { kind: 'loading', channel: 'rgba', ...READ_ONLY },
    effects: [{ kind: 'inspect' }],
  };
}

/** One intent or native answer applied without reaching out to either side of the process boundary. */
export function updateTexture(
  state: TextureEditorState,
  event: TextureEditorEvent,
): TextureEditorUpdate {
  switch (event.kind) {
    case 'inspected':
      return inspected(state, event.inspection);
    case 'inspection-failed':
      return {
        state: { kind: 'failed', channel: state.channel, reason: event.reason, ...READ_ONLY },
        effects: [],
      };
    case 'select-channel':
      return selectedChannel(state, event.channel);
    case 'select-mip':
      return selectedMip(state, event.mip);
    case 'previewed':
      return previewed(state, event.request, event.preview);
    case 'preview-failed':
      return previewFailed(state, event.request, event.reason);
    case 'reconversion-available':
      return {
        state: { ...state, reconvert: { kind: 'available', source: event.source } },
        effects: [],
      };
    case 'reconversion-refused':
      return {
        state: { ...state, reconvert: { kind: 'refused', reason: event.reason } },
        effects: [],
      };
  }
}

function inspected(state: TextureEditorState, inspection: EddsInspection): TextureEditorUpdate {
  if (state.kind !== 'loading') {
    return unchanged(state);
  }

  const first = inspection.mips[0];
  if (first === undefined) {
    return {
      state: {
        kind: 'failed',
        channel: state.channel,
        reason: 'The EDDS contains no mip levels.',
        ...READ_ONLY,
      },
      effects: [],
    };
  }

  if (inspection.pixels.kind === 'unsupported') {
    return {
      state: {
        kind: 'inspect-only',
        channel: state.channel,
        selectedMip: first.level,
        inspection,
        preview: { kind: 'unsupported-format', reason: inspection.pixels.reason },
        nextRequest: 1,
        ...READ_ONLY,
      },
      effects: [],
    };
  }

  return {
    state: {
      kind: 'inspect-only',
      channel: state.channel,
      selectedMip: first.level,
      inspection,
      preview: { kind: 'loading', mip: first.level, request: 1 },
      nextRequest: 2,
      ...READ_ONLY,
    },
    effects: [{ kind: 'preview', mip: first.level, request: 1 }],
  };
}

function selectedChannel(
  state: TextureEditorState,
  channel: TextureChannel,
): TextureEditorUpdate {
  if (state.channel === channel) {
    return unchanged(state);
  }

  return { state: { ...state, channel }, effects: [] };
}

function selectedMip(state: TextureEditorState, mip: number): TextureEditorUpdate {
  if (
    state.kind !== 'inspect-only' ||
    state.selectedMip === mip ||
    !state.inspection.mips.some((candidate) => candidate.level === mip)
  ) {
    return unchanged(state);
  }

  if (state.inspection.pixels.kind === 'unsupported') {
    return { state: { ...state, selectedMip: mip }, effects: [] };
  }

  const request = state.nextRequest;
  return {
    state: {
      ...state,
      selectedMip: mip,
      preview: { kind: 'loading', mip, request },
      nextRequest: request + 1,
    },
    effects: [{ kind: 'preview', mip, request }],
  };
}

function previewed(
  state: TextureEditorState,
  request: number,
  preview: EddsPreview,
): TextureEditorUpdate {
  if (!awaits(state, request) || preview.level !== state.selectedMip) {
    return unchanged(state);
  }

  return { state: { ...state, preview: { kind: 'ready', preview } }, effects: [] };
}

function previewFailed(
  state: TextureEditorState,
  request: number,
  reason: string,
): TextureEditorUpdate {
  if (!awaits(state, request)) {
    return unchanged(state);
  }

  return { state: { ...state, preview: { kind: 'failed', reason } }, effects: [] };
}

function awaits(state: TextureEditorState, request: number): state is InspectedTexture {
  return (
    state.kind === 'inspect-only' &&
    state.preview.kind === 'loading' &&
    state.preview.request === request
  );
}

function unchanged(state: TextureEditorState): TextureEditorUpdate {
  return { state, effects: [] };
}
