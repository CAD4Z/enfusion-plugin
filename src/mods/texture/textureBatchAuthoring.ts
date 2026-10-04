import { textureHdrRefusalOf } from './textureHdr';
import { textureSwizzleRefusalOf } from './textureSwizzles';
import type { EddsConversion } from './edds';
import type { TextureRendering } from './textureAuthoring';
import {
  type TextureBatchPlan,
  type TextureBatchJob,
  withTextureBatchProfile,
} from './textureBatch';
import type { TextureBatchEvent } from './textureBatchProtocol';
import {
  withActiveMipSettings,
  type ArtifactRevision,
  type TextureProfile,
} from './textureConversion';
import {
  isTextureQuality,
  textureConversionCapabilityOf,
} from './textureConversions';
import { samePath } from '../paths';

type ReadyBatch = Extract<TextureBatchPlan, { kind: 'ready' }>;
type ReadyPlan = ReadyBatch['jobs'][number];

export interface TextureBatchRuntimeItem {
  readonly id: string;
  readonly source: string;
  readonly plan: ReadyPlan;
  readonly status: 'Queued' | 'Converting' | 'Converted' | 'Failed' | 'Cancelled';
  readonly progress: number;
  readonly reason?: string;
  readonly retryable?: boolean;
  readonly conversion?: EddsConversion;
}

export type TextureBatchAuthoringState =
  | { readonly kind: 'loading' }
  | { readonly kind: 'refused'; readonly reason: string }
  | {
      readonly kind: 'authoring';
      readonly plan: ReadyBatch;
      readonly draft: TextureProfile;
      readonly revision: number;
      readonly activeSource: string;
      readonly preview:
        | { readonly kind: 'loading' }
        | { readonly kind: 'ready'; readonly rendered: TextureRendering }
        | { readonly kind: 'unavailable'; readonly reason: string };
    }
  | {
      readonly kind: 'running';
      readonly plan: ReadyBatch;
      readonly draft: TextureProfile;
      readonly activeSource: string;
      readonly rendered?: TextureRendering;
      readonly items: readonly TextureBatchRuntimeItem[];
      readonly cancelling: boolean;
      readonly diagnostics: readonly Extract<TextureBatchEvent, { kind: 'diagnostic' }>[];
    }
  | {
      readonly kind: 'result';
      readonly plan: ReadyBatch;
      readonly draft: TextureProfile;
      readonly activeSource: string;
      readonly rendered?: TextureRendering;
      readonly items: readonly TextureBatchRuntimeItem[];
      readonly diagnostics: readonly Extract<TextureBatchEvent, { kind: 'diagnostic' }>[];
      readonly checkingRetry: boolean;
      readonly retryReason?: string;
    };

type EditableBatchProfileKey =
  | 'FormatCompress'
  | 'CompressTreshold'
  | 'RemoveMips'
  | 'ContainsMips'
  | 'GenerateMips'
  | 'GenerateCubemap'
  | 'Normalize'
  | 'TiledTexture'
  | 'MipMapFunction'
  | 'MipMapFilter'
  | 'Conversion'
  | 'ConversionQuality'
  | 'Swizzling';

type TextureBatchProfileChange = {
  readonly [Key in EditableBatchProfileKey]: {
    readonly kind: 'change-profile';
    readonly field: Key;
    readonly value: TextureProfile[Key];
  };
}[EditableBatchProfileKey];

export type TextureBatchAuthoringEvent =
  | { readonly kind: 'loaded'; readonly plan: TextureBatchPlan }
  | { readonly kind: 'load-failed'; readonly reason: string }
  | { readonly kind: 'select-item'; readonly source: string }
  | TextureBatchProfileChange
  | { readonly kind: 'item-rendered'; readonly source: string; readonly revision: number; readonly rendered: TextureRendering }
  | { readonly kind: 'item-render-failed'; readonly source: string; readonly revision: number; readonly reason: string }
  | { readonly kind: 'run' }
  | { readonly kind: 'native-event'; readonly event: TextureBatchEvent }
  /**
   * A failure of the batch itself rather than of one job. A process that died mid-flight leaves
   * work worth retrying; a refused job count, a duplicated id or a broken stream is the same
   * input every time, so it must not come back as retryable work.
   */
  | { readonly kind: 'batch-failed'; readonly reason: string; readonly retryable: boolean }
  | { readonly kind: 'cancel' }
  | { readonly kind: 'retry-failed' }
  | { readonly kind: 'retry-refreshed'; readonly plan: TextureBatchPlan }
  | { readonly kind: 'retry-refresh-failed'; readonly reason: string };

export type TextureBatchAuthoringEffect =
  | { readonly kind: 'load' }
  | {
      readonly kind: 'render-item';
      readonly source: string;
      readonly revision: number;
      readonly plan: ReadyPlan;
      readonly profile: TextureProfile;
    }
  | { readonly kind: 'convert-batch'; readonly jobs: readonly TextureBatchJob[] }
  | { readonly kind: 'refresh-retry' }
  | { readonly kind: 'cancel-batch' };

export interface TextureBatchAuthoringUpdate {
  readonly state: TextureBatchAuthoringState;
  readonly effects: readonly TextureBatchAuthoringEffect[];
}

export function openedTextureBatch(): TextureBatchAuthoringUpdate {
  return { state: { kind: 'loading' }, effects: [{ kind: 'load' }] };
}

export function updateTextureBatch(
  state: TextureBatchAuthoringState,
  event: TextureBatchAuthoringEvent,
): TextureBatchAuthoringUpdate {
  switch (event.kind) {
    case 'loaded': return loaded(state, event.plan);
    case 'load-failed':
      return state.kind === 'loading'
        ? { state: { kind: 'refused', reason: event.reason }, effects: [] }
        : unchanged(state);
    case 'select-item': return selected(state, event.source);
    case 'change-profile': return changed(state, event);
    case 'item-rendered': return rendered(state, event);
    case 'item-render-failed': return renderFailed(state, event);
    case 'run': return run(state);
    case 'native-event': return nativeEvent(state, event.event);
    case 'batch-failed': return batchFailed(state, event.reason, event.retryable);
    case 'cancel':
      return state.kind === 'running' && !state.cancelling
        ? { state: { ...state, cancelling: true }, effects: [{ kind: 'cancel-batch' }] }
        : unchanged(state);
    case 'retry-failed': return retryFailed(state);
    case 'retry-refreshed': return retryRefreshed(state, event.plan);
    case 'retry-refresh-failed':
      return state.kind === 'result' && state.checkingRetry
        ? { state: { ...state, checkingRetry: false, retryReason: event.reason }, effects: [] }
        : unchanged(state);
  }
}

function loaded(
  state: TextureBatchAuthoringState,
  plan: TextureBatchPlan,
): TextureBatchAuthoringUpdate {
  if (state.kind !== 'loading') return unchanged(state);
  if (plan.kind === 'refused') return { state: plan, effects: [] };
  const activeSource = plan.primary;
  const active = readyItem(plan, activeSource);
  return {
    state: {
      kind: 'authoring',
      plan,
      draft: plan.profile,
      revision: 1,
      activeSource,
      preview: active === undefined
        ? { kind: 'unavailable', reason: refusalOf(plan, activeSource) }
        : { kind: 'loading' },
    },
    effects: active === undefined ? [] : [renderEffect(activeSource, 1, active)],
  };
}

function selected(
  state: TextureBatchAuthoringState,
  source: string,
): TextureBatchAuthoringUpdate {
  if (state.kind !== 'authoring' || samePath(state.activeSource) === samePath(source) ||
      !state.plan.items.some((item) => samePath(item.source) === samePath(source))) {
    return unchanged(state);
  }
  const revision = state.revision + 1;
  const active = readyItem(state.plan, source);
  return {
    state: {
      ...state,
      revision,
      activeSource: source,
      preview: active === undefined
        ? { kind: 'unavailable', reason: refusalOf(state.plan, source) }
        : { kind: 'loading' },
    },
    effects: active === undefined ? [] : [renderEffect(source, revision, active)],
  };
}

function changed(
  state: TextureBatchAuthoringState,
  event: Extract<TextureBatchAuthoringEvent, { kind: 'change-profile' }>,
): TextureBatchAuthoringUpdate {
  if (state.kind !== 'authoring') {
    return unchanged(state);
  }
  const capability = textureConversionCapabilityOf(
    event.field === 'Conversion' ? event.value : state.draft.Conversion,
  );
  if (state.draft[event.field] === event.value ||
      (event.field === 'CompressTreshold' && (event.value < 0 || event.value > 100)) ||
      (event.field === 'RemoveMips' &&
        (!Number.isInteger(event.value) || event.value < 0 || event.value > 14)) ||
      (event.field === 'ContainsMips' &&
        !state.plan.jobs.every((job) => job.sourceFormat === 'DDS')) ||
      (event.field === 'ConversionQuality' && !isTextureQuality(event.value)) ||
      capability === undefined || !capability.supported ||
      (event.field === 'ConversionQuality' && !capability.usesQuality && event.value !== 1)) {
    return unchanged(state);
  }
  const draft: TextureProfile = withActiveMipSettings({
    ...state.draft,
    [event.field]: event.value,
    ...(event.field === 'ContainsMips' && event.value ? { GenerateMips: false } : {}),
    /* A conversion that cannot use quality carries the default, so the recipe stays runnable. */
    ...(event.field === 'Conversion' && !capability.usesQuality ? { ConversionQuality: 1 } : {}),
  });
  if (textureHdrRefusalOf(draft, state.plan.jobs.map((job) => job.sourceFormat)) !== undefined) return unchanged(state);
  if (textureSwizzleRefusalOf(draft) !== undefined) return unchanged(state);
  const plan = withTextureBatchProfile(state.plan, draft);
  const revision = state.revision + 1;
  const active = readyItem(plan, state.activeSource);
  return {
    state: {
      ...state,
      plan,
      draft,
      revision,
      preview: active === undefined ? state.preview : { kind: 'loading' },
    },
    effects: active === undefined ? [] : [renderEffect(state.activeSource, revision, active)],
  };
}

function rendered(
  state: TextureBatchAuthoringState,
  event: Extract<TextureBatchAuthoringEvent, { kind: 'item-rendered' }>,
): TextureBatchAuthoringUpdate {
  return state.kind === 'authoring' && state.revision === event.revision &&
      samePath(state.activeSource) === samePath(event.source)
    ? { state: { ...state, preview: { kind: 'ready', rendered: event.rendered } }, effects: [] }
    : unchanged(state);
}

function renderFailed(
  state: TextureBatchAuthoringState,
  event: Extract<TextureBatchAuthoringEvent, { kind: 'item-render-failed' }>,
): TextureBatchAuthoringUpdate {
  return state.kind === 'authoring' && state.revision === event.revision &&
      samePath(state.activeSource) === samePath(event.source)
    ? { state: { ...state, preview: { kind: 'unavailable', reason: event.reason } }, effects: [] }
    : unchanged(state);
}

function run(state: TextureBatchAuthoringState): TextureBatchAuthoringUpdate {
  if (state.kind !== 'authoring' || state.plan.jobs.length === 0) return unchanged(state);
  const jobs = jobsOf(state.plan.jobs);
  return {
    state: {
      kind: 'running',
      plan: state.plan,
      draft: state.draft,
      activeSource: state.activeSource,
      rendered: state.preview.kind === 'ready' ? state.preview.rendered : undefined,
      items: jobs.map(({ id, plan }) => ({
        id, source: plan.source, plan, status: 'Queued', progress: 0,
      })),
      cancelling: false,
      diagnostics: [],
    },
    effects: [{ kind: 'convert-batch', jobs }],
  };
}

function nativeEvent(
  state: TextureBatchAuthoringState,
  event: TextureBatchEvent,
): TextureBatchAuthoringUpdate {
  if (state.kind !== 'running') return unchanged(state);
  if (event.kind === 'diagnostic') {
    return { state: { ...state, diagnostics: [...state.diagnostics, event] }, effects: [] };
  }
  if (event.kind === 'progress') {
    return {
      state: {
        ...state,
        items: state.items.map((item) => item.id === event.id &&
            item.status !== 'Converted' && item.status !== 'Failed' && item.status !== 'Cancelled'
          ? { ...item, status: 'Converting', progress: event.progress }
          : item),
      },
      effects: [],
    };
  }
  if (event.kind === 'result') {
    return {
      state: {
        ...state,
        items: state.items.map((item): TextureBatchRuntimeItem => item.id !== event.id
          ? item
          : event.status === 'Converted'
            ? { ...item, status: 'Converted', progress: 1, conversion: event.conversion }
            : {
                ...item,
                status: event.status,
                reason: event.reason,
                retryable: event.retryable,
              }),
      },
      effects: [],
    };
  }
  if (event.kind === 'complete') {
    return {
      state: {
        kind: 'result',
        plan: state.plan,
        draft: state.draft,
        activeSource: state.activeSource,
        rendered: state.rendered,
        items: state.items.map((item) => item.status === 'Queued' || item.status === 'Converting'
          ? { ...item, status: 'Cancelled', reason: 'The job did not complete.', retryable: true }
          : item),
        diagnostics: state.diagnostics,
        checkingRetry: false,
      },
      effects: [],
    };
  }
  return unchanged(state);
}

function batchFailed(
  state: TextureBatchAuthoringState,
  reason: string,
  retryable: boolean,
): TextureBatchAuthoringUpdate {
  if (state.kind !== 'running') return unchanged(state);
  const cancelled = state.cancelling;
  return {
    state: {
      kind: 'result',
      plan: state.plan,
      draft: state.draft,
      activeSource: state.activeSource,
      rendered: state.rendered,
      items: state.items.map((item) => item.status === 'Queued' || item.status === 'Converting'
        ? {
            ...item,
            status: cancelled ? 'Cancelled' : 'Failed',
            reason,
            retryable: !cancelled && retryable,
          }
        : item),
      diagnostics: state.diagnostics,
      checkingRetry: false,
    },
    effects: [],
  };
}

/**
 * What Retry Failed takes up again: a failure, and a job that never got to finish — which the
 * batch marks cancelled and retryable — but never one the developer cancelled.
 */
function isRetryCandidate(item: TextureBatchRuntimeItem): boolean {
  return item.status === 'Failed' || (item.status === 'Cancelled' && item.retryable === true);
}

/** How many items Retry Failed would take up again in this state. */
export function textureBatchRetryCount(state: TextureBatchAuthoringState): number {
  return state.kind === 'result' ? state.items.filter(isRetryCandidate).length : 0;
}

function retryFailed(state: TextureBatchAuthoringState): TextureBatchAuthoringUpdate {
  if (state.kind !== 'result' || state.checkingRetry ||
      !state.items.some(isRetryCandidate)) return unchanged(state);
  return {
    state: { ...state, checkingRetry: true, retryReason: undefined },
    effects: [{ kind: 'refresh-retry' }],
  };
}

function retryRefreshed(
  state: TextureBatchAuthoringState,
  refreshed: TextureBatchPlan,
): TextureBatchAuthoringUpdate {
  if (state.kind !== 'result' || !state.checkingRetry) return unchanged(state);
  if (refreshed.kind === 'refused') {
    return {
      state: { ...state, checkingRetry: false, retryReason: refreshed.reason },
      effects: [],
    };
  }
  const plan = withTextureBatchProfile(refreshed, state.draft);
  const retry = state.items.flatMap((item): TextureBatchRuntimeItem[] => {
    if (!isRetryCandidate(item)) return [];
    const current = readyItem(plan, item.source);
    if (current === undefined || (item.retryable !== true && !revisionsChanged(item.plan, current))) {
      return [];
    }
    // The refresh exists to re-snapshot revisions, so a retry runs against what is on disk now.
    return [{
      ...item,
      plan: current,
      status: 'Queued',
      progress: 0,
      reason: undefined,
      retryable: undefined,
    }];
  });
  if (retry.length === 0) {
    return {
      state: {
        ...state,
        plan,
        checkingRetry: false,
        retryReason: 'No failed input changed or became retryable.',
      },
      effects: [],
    };
  }
  const byId = new Map(retry.map((item) => [item.id, item]));
  return {
    state: {
      kind: 'running',
      plan,
      draft: state.draft,
      activeSource: state.activeSource,
      rendered: state.rendered,
      items: state.items.map((item) => byId.get(item.id) ?? item),
      cancelling: false,
      diagnostics: [],
    },
    effects: [{ kind: 'convert-batch', jobs: retry.map(({ id, plan: item }) => ({ id, plan: item })) }],
  };
}

export function textureBatchProgress(state: TextureBatchAuthoringState): number {
  if (state.kind !== 'running' && state.kind !== 'result') return 0;
  return state.items.length === 0
    ? 0
    : state.items.reduce((sum, item) => sum + item.progress, 0) / state.items.length;
}

function jobsOf(plans: readonly ReadyPlan[]): TextureBatchJob[] {
  return plans.map((plan, at) => ({ id: String(at), plan }));
}

function readyItem(plan: ReadyBatch, source: string): ReadyPlan | undefined {
  const item = plan.items.find((candidate) => samePath(candidate.source) === samePath(source));
  return item?.kind === 'ready' ? item.plan : undefined;
}

function revisionsChanged(left: ReadyPlan, right: ReadyPlan): boolean {
  return !sameRevision(left.revisions.source, right.revisions.source) ||
    !sameRevision(left.revisions.output, right.revisions.output) ||
    !sameRevision(left.revisions.metadata, right.revisions.metadata);
}

function sameRevision(left: ArtifactRevision | undefined, right: ArtifactRevision | undefined): boolean {
  return left === right || (left?.size === right?.size && left?.modified === right?.modified);
}

function refusalOf(plan: ReadyBatch, source: string): string {
  const item = plan.items.find((candidate) => samePath(candidate.source) === samePath(source));
  return item?.kind === 'refused' ? item.reason : 'This item cannot be previewed.';
}

function renderEffect(source: string, revision: number, plan: ReadyPlan): TextureBatchAuthoringEffect {
  return { kind: 'render-item', source, revision, plan, profile: plan.profile };
}

function unchanged(state: TextureBatchAuthoringState): TextureBatchAuthoringUpdate {
  return { state, effects: [] };
}
