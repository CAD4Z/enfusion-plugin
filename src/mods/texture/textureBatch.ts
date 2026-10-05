import { resolveWindows, samePath, windowsFolder } from '../paths';
import {
  TEXTURE_PRIMARY_REFUSAL,
  isTextureSourcePath,
  textureSourceFormatOf,
} from './textureSources';
import { BATCH_MAX_JOBS } from './textureBatchProtocol';
import { textureHdrRefusalOf } from './textureHdr';
import {
  DEFAULT_TEXTURE_PROFILE,
  type ArtifactRevision,
  type EnfusionRoot,
  type ExistingTextureMetadata,
  type TextureConversionPlan,
  type TextureProfile,
  textureConversionPlanOf,
  textureScopeOf,
} from './textureConversion';

export interface TextureBatchItemInput {
  readonly source: string;
  readonly kind: 'file' | 'folder' | 'missing';
  readonly sourceRevision?: ArtifactRevision;
  readonly outputRevision?: ArtifactRevision;
  readonly metadata: ExistingTextureMetadata;
  readonly newGuid: string;
}

export interface TextureBatchInput {
  /** The Explorer context target; selection order never chooses the primary source image. */
  readonly primary: string;
  readonly roots: readonly EnfusionRoot[];
  readonly items: readonly TextureBatchItemInput[];
  readonly occupiedGuids: readonly string[];
}

export type TextureBatchItemPlan =
  | {
      readonly source: string;
      readonly kind: 'ready';
      readonly action: 'convert' | 'reconvert' | 'replace';
      readonly label: 'Convert' | 'Reconvert' | 'Replace';
      readonly plan: Extract<TextureConversionPlan, { kind: 'ready' }>;
    }
  | {
      readonly source: string;
      readonly kind: 'refused';
      readonly reason: string;
      /** Paths and identity survive a profile refusal, so a compatible edit can restore the item. */
      readonly candidate?: Extract<TextureConversionPlan, { kind: 'ready' }>;
    };

export type TextureBatchPlan =
  | { readonly kind: 'refused'; readonly reason: string }
  | {
      readonly kind: 'ready';
      readonly primary: string;
      readonly profile: TextureProfile;
      readonly items: readonly TextureBatchItemPlan[];
      readonly jobs: readonly Extract<TextureConversionPlan, { kind: 'ready' }>[];
    };

export interface TextureBatchJob {
  readonly id: string;
  readonly plan: Extract<TextureConversionPlan, { kind: 'ready' }>;
}

/** Settles an Explorer selection into one immutable, deterministically reported batch. */
export function textureBatchPlanOf(input: TextureBatchInput): TextureBatchPlan {
  const unique = input.items
    .filter((item, at, all) =>
      all.findIndex((candidate) => samePath(candidate.source) === samePath(item.source)) === at)
    .sort((left, right) => samePath(left.source).localeCompare(samePath(right.source)));
  const primary = unique.find((item) => samePath(item.source) === samePath(input.primary));
  if (primary === undefined) {
    return { kind: 'refused', reason: 'The Explorer context target is not in the captured selection.' };
  }

  if (primary.kind !== 'file' || primary.sourceRevision === undefined) {
    return { kind: 'refused', reason: 'The Explorer context target is not a readable source image.' };
  }
  if (textureScopeOf(primary.source, input.roots) === undefined) {
    return { kind: 'refused', reason: 'The primary source is outside every discovered Enfusion root.' };
  }
  if (!isTextureSourcePath(primary.source)) {
    return { kind: 'refused', reason: TEXTURE_PRIMARY_REFUSAL };
  }
  if (primary.metadata.kind === 'invalid' ||
      (primary.metadata.kind === 'unsupported' && metadataOwnsSource(primary))) {
    return {
      kind: 'refused',
      reason: `The primary texture profile is not readable: ${primary.metadata.reason}`,
    };
  }

  const profile = primaryProfileOf(primary);
  const items: TextureBatchItemPlan[] = unique.map((item) => {
    if (item.kind !== 'file' || item.sourceRevision === undefined) {
      return {
        source: item.source,
        kind: 'refused',
        reason: item.kind === 'folder'
          ? 'Folders are not expanded by texture conversion.'
          : 'The source image could not be read.',
      };
    }
    const plan = textureConversionPlanOf({
      source: item.source,
      roots: input.roots,
      sourceRevision: item.sourceRevision,
      outputRevision: item.outputRevision,
      metadata: metadataForBatch(item),
      newGuid: item.newGuid,
      occupiedGuids: input.occupiedGuids,
    });
    return plan.kind === 'ready'
      ? {
          source: item.source,
          kind: 'ready',
          action: plan.action,
          label: plan.label,
          plan,
        }
      : { source: item.source, kind: 'refused', reason: plan.reason };
  });
  const destinations = new Map<string, number>();
  for (const item of unique) {
    const destination = collisionDestinationOf(item, input.roots);
    if (destination !== undefined) {
      destinations.set(destination, (destinations.get(destination) ?? 0) + 1);
    }
  }
  const isolated = items.map((item): TextureBatchItemPlan =>
    item.kind === 'ready' && (destinations.get(samePath(item.plan.output)) ?? 0) > 1
      ? {
          source: item.source,
          kind: 'refused',
          reason: 'Multiple selected sources resolve to the same EDDS output.',
        }
      : item);
  const jobs = isolated.flatMap((item) => item.kind === 'ready' ? [item.plan] : []);
  if (jobs.length > BATCH_MAX_JOBS) {
    return {
      kind: 'refused',
      reason: `A conversion batch runs at most ${BATCH_MAX_JOBS} textures at once; this selection has ${jobs.length}.`,
    };
  }
  return withTextureBatchProfile({ kind: 'ready', primary: primary.source, profile, items: isolated, jobs }, profile);
}

/** Rechecks profile compatibility without changing captured paths, identity, revisions or collisions. */
export function withTextureBatchProfile(
  plan: Extract<TextureBatchPlan, { kind: 'ready' }>,
  profile: TextureProfile,
): Extract<TextureBatchPlan, { kind: 'ready' }> {
  const items = plan.items.map((item): TextureBatchItemPlan => {
    const previous = candidateOf(item);
    if (previous === undefined) return item;
    const candidate = { ...previous, profile };
    const reason = textureHdrRefusalOf(profile, candidate.sourceFormat);
    return reason === undefined
      ? { source: item.source, kind: 'ready', action: candidate.action, label: candidate.label, plan: candidate }
      : { source: item.source, kind: 'refused', reason, candidate };
  });
  return {
    ...plan,
    profile,
    items,
    jobs: items.flatMap((item) => item.kind === 'ready' ? [item.plan] : []),
  };
}

/** Controls must also consider sources temporarily excluded by the current profile. */
export function textureBatchSourceFormatsOf(plan: Extract<TextureBatchPlan, { kind: 'ready' }>) {
  return plan.items.flatMap((item) => {
    const candidate = candidateOf(item);
    return candidate === undefined ? [] : [candidate.sourceFormat];
  });
}

function candidateOf(item: TextureBatchItemPlan) {
  return item.kind === 'ready' ? item.plan : item.candidate;
}

function primaryProfileOf(primary: TextureBatchItemInput): TextureProfile {
  if (primary.metadata.kind !== 'valid' || !metadataOwnsSource(primary)) {
    return DEFAULT_TEXTURE_PROFILE;
  }
  return primary.metadata.value.profile;
}

function metadataOwnsSource(item: TextureBatchItemInput): boolean {
  if (item.metadata.kind !== 'valid' && item.metadata.kind !== 'unsupported') return false;
  const output = item.source.slice(0, item.source.lastIndexOf('.')) + '.edds';
  const identity = item.metadata.kind === 'valid' ? item.metadata.value : item.metadata.identity;
  const owner = resolveWindows(windowsFolder(output), identity.sourceFile);
  return samePath(owner) === samePath(item.source);
}

function collisionDestinationOf(
  item: TextureBatchItemInput,
  roots: readonly EnfusionRoot[],
): string | undefined {
  return item.kind === 'file' && item.sourceRevision !== undefined &&
      isTextureSourcePath(item.source) && textureScopeOf(item.source, roots) !== undefined
    ? samePath(item.source.slice(0, item.source.lastIndexOf('.')) + '.edds')
    : undefined;
}

/** Plan ownership with neutral settings; only the common profile determines batch compatibility. */
function metadataForBatch(item: TextureBatchItemInput): ExistingTextureMetadata {
  const sourceFormat = textureSourceFormatOf(item.source);
  if (item.metadata.kind === 'valid') {
    return { ...item.metadata, value: { ...item.metadata.value, profile: DEFAULT_TEXTURE_PROFILE } };
  }
  if (item.metadata.kind !== 'unsupported' || sourceFormat === undefined) {
    return item.metadata;
  }
  return {
    kind: 'valid',
    revision: item.metadata.revision,
    value: {
      ...item.metadata.identity,
      sourceFormat,
      profile: DEFAULT_TEXTURE_PROFILE,
    },
  };
}
