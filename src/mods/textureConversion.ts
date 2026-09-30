import { resolveWindows, samePath, windowsFolder, windowsName } from './paths';
import {
  TEXTURE_SOURCE_REFUSAL,
  type TextureSourceFormat,
  textureSourceFormatOf,
} from './textureSources';
import type { TextureConversion } from './textureConversions';

export type { TextureSourceFormat };
export type TextureCompression = 'Copy' | 'Fastest' | 'Medium' | 'Best';
export type TextureMipFunction = 'Filter' | 'Normalize' | 'ColorNoise';
export type TextureMipFilter = 'Box' | 'Kaiser';

/** The entire supported Workbench recipe. There are no implicit preset fields. */
export interface TextureProfile {
  readonly TargetFormat: 'EnfusionDDS';
  readonly FormatCompress: TextureCompression;
  readonly CompressTreshold: number;
  /** Larger mip levels removed after a generated or supplied chain is complete. */
  readonly RemoveMips: number;
  readonly Conversion: TextureConversion;
  /** A fraction of one, to three decimals; only a compressed conversion reads it. */
  readonly ConversionQuality: number;
  readonly Swizzling: 'None';
  /** True only when the DDS source proves a complete supplied mip chain. */
  readonly ContainsMips: boolean;
  readonly GenerateMips: boolean;
  /** Normalizes the source before the chain stage; distinct from MipMapFunction=Normalize. */
  readonly Normalize: boolean;
  readonly MipMapFunction: TextureMipFunction;
  readonly MipMapFilter: TextureMipFilter;
  readonly TiledTexture: boolean;
}

export const DEFAULT_TEXTURE_PROFILE: TextureProfile = {
  TargetFormat: 'EnfusionDDS',
  FormatCompress: 'Fastest',
  CompressTreshold: 80,
  RemoveMips: 0,
  Conversion: 'None',
  ConversionQuality: 1,
  Swizzling: 'None',
  ContainsMips: false,
  GenerateMips: true,
  Normalize: false,
  MipMapFunction: 'Filter',
  MipMapFilter: 'Box',
  TiledTexture: true,
};

/** Keeps disabled mip controls at their canonical defaults across single and batch authoring. */
export function withActiveMipSettings(profile: TextureProfile): TextureProfile {
  if (!profile.GenerateMips) {
    return { ...profile, MipMapFunction: 'Filter', MipMapFilter: 'Box' };
  }
  return profile.MipMapFunction !== 'Normalize'
    ? profile
    : { ...profile, MipMapFilter: 'Box' };
}

export interface TextureIdentity {
  readonly guid: string;
  readonly name: string;
  readonly sourceFile: string;
}

export interface TextureMetadata extends TextureIdentity {
  readonly sourceFormat: TextureSourceFormat;
  readonly profile: TextureProfile;
}

export interface ArtifactRevision {
  readonly size: number;
  readonly modified: number;
}

export interface EnfusionRoot {
  /** A folder established by mod.enf, workspace.enf, or an enclosing discovered mod. */
  readonly root: string;
  /** The registered resource namespace below that root, when one was discovered. */
  readonly prefixRoot?: string;
}

export type ExistingTextureMetadata =
  | { readonly kind: 'missing' }
  | {
      readonly kind: 'invalid';
      readonly revision: ArtifactRevision;
      readonly reason: string;
    }
  | {
      /** Identity parsed safely, while the old recipe is outside the supported conversion slice. */
      readonly kind: 'unsupported';
      readonly revision: ArtifactRevision;
      readonly identity: TextureIdentity;
      readonly reason: string;
    }
  | {
      readonly kind: 'valid';
      readonly revision: ArtifactRevision;
      readonly value: TextureMetadata;
    };

export interface TextureConversionInput {
  readonly source: string;
  readonly roots: readonly EnfusionRoot[];
  readonly sourceRevision: ArtifactRevision;
  readonly outputRevision?: ArtifactRevision;
  readonly metadata: ExistingTextureMetadata;
  /** Random candidate supplied by the adapter only when a new registered identity is needed. */
  readonly newGuid: string;
  /** Every GUID in the authoritative registration scope, for collision refusal. */
  readonly occupiedGuids: readonly string[];
}

export type TextureConversionPlan =
  | { readonly kind: 'refused'; readonly reason: string }
  | {
      readonly kind: 'ready';
      readonly scope: 'registered' | 'detached';
      readonly action: 'convert' | 'reconvert' | 'replace';
      readonly label: 'Convert' | 'Reconvert' | 'Replace';
      readonly source: string;
      readonly sourceFormat: TextureSourceFormat;
      readonly output: string;
      readonly metadata?: string;
      readonly identity?: TextureIdentity;
      readonly identityAction: 'create' | 'preserve' | 'none';
      readonly profile: TextureProfile;
      readonly revisions: {
        readonly source: ArtifactRevision;
        readonly output?: ArtifactRevision;
        readonly metadata?: ArtifactRevision;
      };
      readonly notice?: string;
    };

/**
 * Settles scope, ownership and every write as one immutable value before a preview or conversion
 * starts. The platform layer supplies filesystem facts; this function neither reads nor writes.
 */
export function textureConversionPlanOf(input: TextureConversionInput): TextureConversionPlan {
  const sourceFormat = textureSourceFormatOf(input.source);
  if (sourceFormat === undefined) {
    return { kind: 'refused', reason: TEXTURE_SOURCE_REFUSAL };
  }

  const root = authoritativeRootOf(input.source, input.roots);
  if (root === undefined) {
    return { kind: 'refused', reason: 'The source is outside every discovered Enfusion root.' };
  }

  if (input.metadata.kind === 'invalid' || input.metadata.kind === 'unsupported') {
    return {
      kind: 'refused',
      reason: `The existing metadata is not safe to replace: ${input.metadata.reason}`,
    };
  }

  const output = input.source.slice(0, input.source.lastIndexOf('.')) + '.edds';
  const registered = root.prefixRoot !== undefined && isWithinPath(input.source, root.prefixRoot);
  if (!registered && input.metadata.kind !== 'missing') {
    return {
      kind: 'refused',
      reason: 'A detached conversion cannot replace an EDDS that has registration metadata.',
    };
  }

  const owner = input.metadata.kind === 'valid' ? input.metadata.value : undefined;
  if (owner !== undefined && !/^[0-9A-Fa-f]{16}$/.test(owner.guid)) {
    return {
      kind: 'refused',
      reason: 'The existing metadata is not safe to replace: its GUID is not 64-bit hexadecimal.',
    };
  }

  const action = actionOf(input, output);
  const profile = action === 'reconvert' && owner !== undefined
    ? owner.profile
    : DEFAULT_TEXTURE_PROFILE;
  const revisions = {
    source: input.sourceRevision,
    ...(input.outputRevision === undefined ? {} : { output: input.outputRevision }),
    ...(input.metadata.kind === 'missing' ? {} : { metadata: input.metadata.revision }),
  };

  if (!registered) {
    return {
      kind: 'ready',
      scope: 'detached',
      action,
      label: labelOf(action),
      source: input.source,
      sourceFormat,
      output,
      metadata: undefined,
      identity: undefined,
      identityAction: 'none',
      profile,
      revisions,
      notice: 'Registration was skipped because the source is outside an Enfusion prefix root.',
    };
  }

  const guid = owner?.guid ?? input.newGuid;
  if (owner === undefined && !/^[0-9A-F]{16}$/.test(guid)) {
    return { kind: 'refused', reason: 'A new texture GUID must be uppercase 64-bit hexadecimal.' };
  }
  if (
    owner === undefined &&
    input.occupiedGuids.some((occupied) => occupied.toUpperCase() === guid)
  ) {
    return { kind: 'refused', reason: 'The new texture GUID collides with existing metadata.' };
  }

  const prefixRoot = root.prefixRoot;
  const identity: TextureIdentity = {
    guid,
    name: resourceNameOf(output, prefixRoot),
    sourceFile: windowsName(input.source),
  };

  return {
    kind: 'ready',
    scope: 'registered',
    action,
    label: labelOf(action),
    source: input.source,
    sourceFormat,
    output,
    metadata: `${output}.meta`,
    identity,
    identityAction: owner === undefined ? 'create' : 'preserve',
    profile,
    revisions,
    notice: undefined,
  };
}

/** The same authoritative ancestor decision used by the command gate and the complete plan. */
export function textureScopeOf(
  source: string,
  roots: readonly EnfusionRoot[],
): 'registered' | 'detached' | undefined {
  const root = authoritativeRootOf(source, roots);
  if (root === undefined) return undefined;
  return root.prefixRoot !== undefined && isWithinPath(source, root.prefixRoot)
    ? 'registered'
    : 'detached';
}

/** Generates exactly eight random bytes, retrying collisions in the supplied registration scope. */
export function textureGuidOf(
  randomBytes: () => Uint8Array,
  occupiedGuids: readonly string[],
): string {
  const occupied = new Set(occupiedGuids.map((guid) => guid.toUpperCase()));
  for (let attempt = 0; attempt < 4_096; attempt += 1) {
    const bytes = randomBytes();
    if (bytes.length !== 8) {
      throw new Error('A texture GUID requires exactly eight random bytes.');
    }

    const guid = [...bytes].map((byte) => byte.toString(16).padStart(2, '0')).join('').toUpperCase();
    if (!occupied.has(guid)) {
      return guid;
    }
  }

  throw new Error('Could not generate a unique texture GUID.');
}

function authoritativeRootOf(
  source: string,
  roots: readonly EnfusionRoot[],
): EnfusionRoot | undefined {
  return roots
    .filter((root) => isWithinPath(source, root.root))
    .sort((left, right) => samePath(right.root).length - samePath(left.root).length)[0];
}

function isWithinPath(path: string, root: string): boolean {
  const candidate = samePath(path);
  const ancestor = samePath(root);
  return candidate === ancestor || candidate.startsWith(`${ancestor}/`);
}

function actionOf(
  input: TextureConversionInput,
  output: string,
): 'convert' | 'reconvert' | 'replace' {
  if (input.metadata.kind === 'valid') {
    const ownerSource = resolveWindows(windowsFolder(output), input.metadata.value.sourceFile);
    return samePath(ownerSource) === samePath(input.source) ? 'reconvert' : 'replace';
  }

  return input.outputRevision === undefined ? 'convert' : 'replace';
}

function labelOf(action: 'convert' | 'reconvert' | 'replace') {
  return action === 'convert' ? 'Convert' : action === 'reconvert' ? 'Reconvert' : 'Replace';
}

function resourceNameOf(output: string, prefixRoot: string): string {
  const slashedOutput = output.replace(/\\/g, '/');
  const slashedParent = windowsFolder(prefixRoot).replace(/\\/g, '/').replace(/\/+$/, '');
  return slashedOutput.slice(slashedParent.length + 1);
}
