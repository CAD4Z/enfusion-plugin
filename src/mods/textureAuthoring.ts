import type { EddsConversion, EddsInspection, EddsPreview } from './edds';
import type {
  TextureCompression,
  TextureConversionPlan,
  TextureProfile,
} from './textureConversion';

type ReadyPlan = Extract<TextureConversionPlan, { kind: 'ready' }>;

export interface TextureRendering {
  readonly inspection: EddsInspection;
  /** Original source samples decoded by the native source pipeline. */
  readonly source: EddsPreview;
  /** Runtime pixels decoded back from the temporary or committed EDDS. */
  readonly result: EddsPreview;
}

export const TEXTURE_PROFILE_FIELDS: readonly {
  readonly key: keyof TextureProfile;
  readonly editable: boolean;
  readonly reason?: string;
}[] = [
  {
    key: 'TargetFormat',
    editable: false,
    reason: 'The first conversion slice supports EnfusionDDS only.',
  },
  { key: 'FormatCompress', editable: true },
  { key: 'CompressTreshold', editable: true },
  {
    key: 'Conversion',
    editable: false,
    reason: 'The first conversion slice supports None only.',
  },
  {
    key: 'ConversionQuality',
    editable: false,
    reason: 'Conversion=None fixes ConversionQuality at 1.',
  },
  {
    key: 'Swizzling',
    editable: false,
    reason: 'The first conversion slice supports None only.',
  },
  { key: 'GenerateMips', editable: true },
  {
    key: 'MipMapFunction',
    editable: false,
    reason: 'GenerateMips uses Filter in the first conversion slice.',
  },
  {
    key: 'MipMapFilter',
    editable: false,
    reason: 'MipMapFunction=Filter uses Box in the first conversion slice.',
  },
  {
    key: 'TiledTexture',
    editable: false,
    reason: 'TiledTexture=false is not supported in the first conversion slice.',
  },
];

export type TextureAuthoringState =
  | { readonly kind: 'loading' }
  | { readonly kind: 'refused'; readonly reason: string }
  | {
      readonly kind: 'authoring';
      readonly plan: ReadyPlan;
      readonly draft: TextureProfile;
      readonly revision: number;
      readonly selectedMip: number;
      readonly preview:
        | { readonly kind: 'loading' }
        | { readonly kind: 'ready'; readonly rendered: TextureRendering }
        | { readonly kind: 'failed'; readonly reason: string };
    }
  | {
      readonly kind: 'running';
      readonly plan: ReadyPlan;
      readonly profile: TextureProfile;
      readonly revision: number;
      readonly rendered: TextureRendering;
    }
  | {
      readonly kind: 'result';
      readonly plan: ReadyPlan;
      readonly profile: TextureProfile;
      readonly revision: number;
      readonly conversion: EddsConversion;
      readonly rendered: TextureRendering;
    };

export type TextureAuthoringEvent =
  | { readonly kind: 'loaded'; readonly plan: TextureConversionPlan }
  | { readonly kind: 'load-failed'; readonly reason: string }
  | {
      readonly kind: 'change-profile';
      readonly field: 'FormatCompress';
      readonly value: TextureCompression;
    }
  | {
      readonly kind: 'change-profile';
      readonly field: 'CompressTreshold';
      readonly value: number;
    }
  | {
      readonly kind: 'change-profile';
      readonly field: 'GenerateMips';
      readonly value: boolean;
    }
  | { readonly kind: 'draft-rendered'; readonly revision: number; readonly rendered: TextureRendering }
  | { readonly kind: 'draft-failed'; readonly revision: number; readonly reason: string }
  | { readonly kind: 'run' }
  | { readonly kind: 'select-mip'; readonly mip: number }
  | {
      readonly kind: 'converted';
      readonly revision: number;
      readonly conversion: EddsConversion;
      readonly rendered: TextureRendering;
    }
  | { readonly kind: 'conversion-failed'; readonly revision: number; readonly reason: string }
  | { readonly kind: 'artifact-changed'; readonly artifact: 'source' | 'output' | 'metadata' };

export type TextureAuthoringEffect =
  | { readonly kind: 'load' }
  | {
      readonly kind: 'render-draft';
      readonly revision: number;
      readonly mip: number;
      readonly plan: ReadyPlan;
      readonly profile: TextureProfile;
    }
  | {
      readonly kind: 'convert';
      readonly revision: number;
      readonly plan: ReadyPlan;
      readonly profile: TextureProfile;
    };

export interface TextureAuthoringUpdate {
  readonly state: TextureAuthoringState;
  readonly effects: readonly TextureAuthoringEffect[];
}

export function openedTextureAuthoring(): TextureAuthoringUpdate {
  return { state: { kind: 'loading' }, effects: [{ kind: 'load' }] };
}

/** Pure editor transition: native work and filesystem watches leave as explicit effects/events. */
export function updateTextureAuthoring(
  state: TextureAuthoringState,
  event: TextureAuthoringEvent,
): TextureAuthoringUpdate {
  if (event.kind === 'artifact-changed') {
    return {
      state: {
        kind: 'refused',
        reason: `The ${event.artifact} changed after this conversion session loaded. Reload before writing.`,
      },
      effects: [],
    };
  }

  switch (event.kind) {
    case 'loaded':
      return loaded(state, event.plan);
    case 'load-failed':
      return state.kind === 'loading'
        ? { state: { kind: 'refused', reason: event.reason }, effects: [] }
        : unchanged(state);
    case 'change-profile':
      return changedProfile(state, event);
    case 'draft-rendered':
      return drafted(state, event.revision, event.rendered);
    case 'draft-failed':
      return draftFailed(state, event.revision, event.reason);
    case 'run':
      return run(state);
    case 'select-mip':
      return selectedMip(state, event.mip);
    case 'converted':
      return converted(state, event.revision, event.conversion, event.rendered);
    case 'conversion-failed':
      return conversionFailed(state, event.revision, event.reason);
  }
}

function loaded(state: TextureAuthoringState, plan: TextureConversionPlan): TextureAuthoringUpdate {
  if (state.kind !== 'loading') {
    return unchanged(state);
  }
  if (plan.kind === 'refused') {
    return { state: plan, effects: [] };
  }

  return {
    state: {
      kind: 'authoring',
      plan,
      draft: plan.profile,
      revision: 1,
      selectedMip: 0,
      preview: { kind: 'loading' },
    },
    effects: [{ kind: 'render-draft', revision: 1, mip: 0, plan, profile: plan.profile }],
  };
}

function changedProfile(
  state: TextureAuthoringState,
  event: Extract<TextureAuthoringEvent, { kind: 'change-profile' }>,
): TextureAuthoringUpdate {
  if (state.kind !== 'authoring') {
    return unchanged(state);
  }
  if (event.field === 'CompressTreshold' && (event.value < 0 || event.value > 100)) {
    return unchanged(state);
  }
  if (state.draft[event.field] === event.value) {
    return unchanged(state);
  }

  const draft: TextureProfile = { ...state.draft, [event.field]: event.value };
  const revision = state.revision + 1;
  const plan: ReadyPlan = { ...state.plan, profile: draft };
  return {
    state: { ...state, plan, draft, revision, selectedMip: 0, preview: { kind: 'loading' } },
    effects: [{ kind: 'render-draft', revision, mip: 0, plan, profile: draft }],
  };
}

function selectedMip(state: TextureAuthoringState, mip: number): TextureAuthoringUpdate {
  if (
    state.kind !== 'authoring' ||
    state.preview.kind !== 'ready' ||
    state.selectedMip === mip ||
    !state.preview.rendered.inspection.mips.some((candidate) => candidate.level === mip)
  ) {
    return unchanged(state);
  }
  const revision = state.revision + 1;
  return {
    state: { ...state, revision, selectedMip: mip, preview: { kind: 'loading' } },
    effects: [
      { kind: 'render-draft', revision, mip, plan: state.plan, profile: state.draft },
    ],
  };
}

function drafted(
  state: TextureAuthoringState,
  revision: number,
  rendered: TextureRendering,
): TextureAuthoringUpdate {
  return state.kind === 'authoring' && state.revision === revision
    ? { state: { ...state, preview: { kind: 'ready', rendered } }, effects: [] }
    : unchanged(state);
}

function draftFailed(
  state: TextureAuthoringState,
  revision: number,
  reason: string,
): TextureAuthoringUpdate {
  return state.kind === 'authoring' && state.revision === revision
    ? { state: { ...state, preview: { kind: 'failed', reason } }, effects: [] }
    : unchanged(state);
}

function run(state: TextureAuthoringState): TextureAuthoringUpdate {
  if (state.kind !== 'authoring' || state.preview.kind !== 'ready') {
    return unchanged(state);
  }

  const plan: ReadyPlan = { ...state.plan, profile: state.draft };
  return {
    state: {
      kind: 'running',
      plan,
      profile: state.draft,
      revision: state.revision,
      rendered: state.preview.rendered,
    },
    effects: [
      { kind: 'convert', revision: state.revision, plan, profile: state.draft },
    ],
  };
}

function converted(
  state: TextureAuthoringState,
  revision: number,
  conversion: EddsConversion,
  rendered: TextureRendering,
): TextureAuthoringUpdate {
  return state.kind === 'running' && state.revision === revision
    ? {
        state: {
          kind: 'result',
          plan: state.plan,
          profile: state.profile,
          revision,
          conversion,
          rendered,
        },
        effects: [],
      }
    : unchanged(state);
}

function conversionFailed(
  state: TextureAuthoringState,
  revision: number,
  reason: string,
): TextureAuthoringUpdate {
  return state.kind === 'running' && state.revision === revision
    ? { state: { kind: 'refused', reason }, effects: [] }
    : unchanged(state);
}

function unchanged(state: TextureAuthoringState): TextureAuthoringUpdate {
  return { state, effects: [] };
}
