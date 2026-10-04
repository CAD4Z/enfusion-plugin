/** Filesystem facts and native work behind one source-image conversion session. */

import { randomBytes, randomUUID } from 'node:crypto';
import path from 'node:path';
import * as vscode from 'vscode';
import type { EddsConversion } from '../../mods/texture/edds';
import type { TextureRendering } from '../../mods/texture/textureAuthoring';
import {
  type TextureBatchItemInput,
  type TextureBatchPlan,
  textureBatchPlanOf,
} from '../../mods/texture/textureBatch';
import {
  type ArtifactRevision,
  DEFAULT_TEXTURE_PROFILE,
  type EnfusionRoot,
  type ExistingTextureMetadata,
  type TextureConversionPlan,
  textureConversionPlanOf,
  textureGuidOf,
  textureScopeOf,
} from '../../mods/texture/textureConversion';
import {
  TEXTURE_PRIMARY_REFUSAL,
  TEXTURE_SOURCE_EXTENSIONS_EITHER,
  isTextureSourcePath,
} from '../../mods/texture/textureSources';
import { folderOf } from '../../mods/paths';
import { type Discovery, findMods } from '../workspace';
import { EddsConverterError, type EddsConverter } from './eddsConverter';

type ReadyPlan = Extract<TextureConversionPlan, { kind: 'ready' }>;

export type ReconvertSource =
  | { readonly kind: 'available'; readonly source: string }
  | { readonly kind: 'refused'; readonly reason: string };

export async function loadTextureConversion(
  source: vscode.Uri,
  converter: EddsConverter,
): Promise<TextureConversionPlan> {
  const found = await findMods();
  const roots = rootsOf(found);
  const sourceRevision = await requiredRevision(source, 'The source image could not be read.');
  const output = siblingOutput(source);
  const metadataUri = vscode.Uri.file(`${output.fsPath}.meta`);
  const [outputRevision, metadataRevision] = await Promise.all([
    optionalRevision(output),
    optionalRevision(metadataUri),
  ]);
  const metadata = await metadataOf(
    output,
    metadataUri,
    outputRevision,
    metadataRevision,
    converter,
  );
  const occupiedGuids = await occupiedGuidsOf(roots, converter);
  const newGuid = textureGuidOf(() => randomBytes(8), occupiedGuids);

  return textureConversionPlanOf({
    source: source.fsPath,
    roots,
    sourceRevision,
    outputRevision,
    metadata,
    newGuid,
    occupiedGuids,
  });
}

/** Captures one explicit Explorer selection; folders and unsupported items stay as item facts. */
export async function loadTextureBatch(
  primary: vscode.Uri,
  selected: readonly vscode.Uri[],
  converter: EddsConverter,
): Promise<TextureBatchPlan> {
  const roots = rootsOf(await findMods());
  if (primary.scheme !== 'file' || textureScopeOf(primary.fsPath, roots) === undefined) {
    return { kind: 'refused', reason: 'The primary source is outside every discovered Enfusion root.' };
  }
  if (!isTextureSourcePath(primary.fsPath)) {
    return { kind: 'refused', reason: TEXTURE_PRIMARY_REFUSAL };
  }

  const captured = [primary, ...selected].filter(
    (uri, at, all) => all.findIndex((candidate) => sameUri(candidate, uri)) === at,
  );
  const occupiedGuids = await occupiedGuidsOf(roots, converter);
  const allocated = [...occupiedGuids];
  const items: TextureBatchItemInput[] = [];
  for (const uri of captured) {
    const source = uri.scheme === 'file' ? uri.fsPath : uri.toString();
    let stat: vscode.FileStat | undefined;
    try {
      stat = await vscode.workspace.fs.stat(uri);
    } catch {
      stat = undefined;
    }
    if (stat === undefined) {
      items.push({
        source,
        kind: 'missing',
        sourceRevision: undefined,
        metadata: { kind: 'missing' },
        newGuid: textureGuidOf(() => randomBytes(8), allocated),
      });
      continue;
    }
    if (stat.type !== vscode.FileType.File) {
      items.push({
        source,
        kind: 'folder',
        sourceRevision: undefined,
        metadata: { kind: 'missing' },
        newGuid: textureGuidOf(() => randomBytes(8), allocated),
      });
      continue;
    }
    const newGuid = textureGuidOf(() => randomBytes(8), allocated);
    allocated.push(newGuid);
    if (uri.scheme !== 'file' || !isTextureSourcePath(source)) {
      items.push({
        source,
        kind: 'file',
        sourceRevision: { size: stat.size, modified: stat.mtime },
        metadata: { kind: 'missing' },
        newGuid,
      });
      continue;
    }
    const output = siblingOutput(uri);
    const metadataUri = vscode.Uri.file(`${output.fsPath}.meta`);
    const [outputRevision, metadataRevision] = await Promise.all([
      optionalRevision(output),
      optionalRevision(metadataUri),
    ]);
    items.push({
      source,
      kind: 'file',
      sourceRevision: { size: stat.size, modified: stat.mtime },
      outputRevision,
      metadata: await metadataOf(output, metadataUri, outputRevision, metadataRevision, converter),
      newGuid,
    });
  }
  return textureBatchPlanOf({ primary: primary.fsPath, roots, items, occupiedGuids });
}

/** Resolves only a native-validated metadata relationship; sibling source names are never guessed. */
export async function reconvertSourceOf(
  output: vscode.Uri,
  converter: EddsConverter,
): Promise<ReconvertSource> {
  const metadata = vscode.Uri.file(`${output.fsPath}.meta`);
  try {
    const inspection = await converter.inspect(output.fsPath, undefined, metadata.fsPath);
    if (inspection.metadata === undefined) {
      return { kind: 'refused', reason: 'Native inspection returned no texture metadata.' };
    }
    const sourcePath = path.resolve(path.dirname(output.fsPath), inspection.metadata.sourceFile);
    if (
      !isTextureSourcePath(sourcePath) ||
      (await optionalRevision(vscode.Uri.file(sourcePath))) === undefined
    ) {
      return {
        kind: 'refused',
        reason: `The validated metadata source is missing or is not a supported ${TEXTURE_SOURCE_EXTENSIONS_EITHER} image.`,
      };
    }
    const plan = await loadTextureConversion(vscode.Uri.file(sourcePath), converter);
    if (
      plan.kind !== 'ready' ||
      plan.action !== 'reconvert' ||
      !sameFsPath(plan.output, output.fsPath)
    ) {
      return {
        kind: 'refused',
        reason: plan.kind === 'refused' ? plan.reason : 'The metadata does not own this EDDS/source pair.',
      };
    }
    return { kind: 'available', source: sourcePath };
  } catch (error: unknown) {
    return {
      kind: 'refused',
      reason: error instanceof Error ? error.message : String(error),
    };
  }
}

/**
 * Renders two independent native products: a COPY/no-mips source view and the current draft. Both
 * live only under extension storage and are decoded back through the EDDS reader before display.
 */
export async function renderTextureDraft(
  plan: ReadyPlan,
  storage: vscode.Uri,
  converter: EddsConverter,
  mip = 0,
  signal?: AbortSignal,
): Promise<TextureRendering> {
  const folder = vscode.Uri.joinPath(storage, 'texture-preview', randomUUID());
  await vscode.workspace.fs.createDirectory(folder);
  const sourceOutput = vscode.Uri.joinPath(folder, 'source.edds').fsPath;
  const resultOutput = vscode.Uri.joinPath(folder, 'result.edds').fsPath;
  const sourcePlan = detachedPlan(plan, sourceOutput, {
    ...DEFAULT_TEXTURE_PROFILE,
    FormatCompress: 'Copy',
    /*
     * The left pane is the source as it is, so it goes through no conversion at all: putting the
     * drafted one through it would show the encoder's output on both sides and leave nothing to
     * compare it against.
     */
    Conversion: 'None',
    ConversionQuality: 1,
    GenerateMips: false,
  });
  const resultPlan = detachedPlan(plan, resultOutput, plan.profile);

  try {
    const [sourceConversion] = await Promise.all([
      converter.convert(sourcePlan, signal, 'preview'),
      converter.convert(resultPlan, signal, 'preview'),
    ]);
    const inspection = await converter.inspect(resultOutput, signal);
    const [source, result] = await Promise.all([
      converter.preview(sourceOutput, 0, signal),
      converter.preview(resultOutput, mip, signal),
    ]);
    return { inspection, source, result, sourceFacts: {
      width: sourceConversion.width, height: sourceConversion.height,
      hasAlpha: sourceConversion.pixelFormat === 'BGRA8',
    } };
  } finally {
    await vscode.workspace.fs.delete(folder, { recursive: true, useTrash: false }).then(
      () => undefined,
      () => undefined,
    );
  }
}

/** Rechecks every captured fact, then asks native code to commit and decodes the committed result. */
export async function commitTextureConversion(
  plan: ReadyPlan,
  sourcePreview: TextureRendering['source'],
  converter: EddsConverter,
  signal?: AbortSignal,
): Promise<{ readonly conversion: EddsConversion; readonly rendered: TextureRendering }> {
  await assertCurrent(plan);
  const conversion = await converter.convert(plan, signal);
  const inspection = await converter.inspect(plan.output, signal, plan.metadata);
  const result = await converter.preview(plan.output, 0, signal);
  return { conversion, rendered: { inspection, source: sourcePreview, result } };
}

export async function assertCurrent(plan: ReadyPlan): Promise<void> {
  const currentSource = await optionalRevision(vscode.Uri.file(plan.source));
  if (!sameRevision(currentSource, plan.revisions.source)) {
    throw new Error('The source changed after this conversion session loaded. Reload before writing.');
  }

  const currentOutput = await optionalRevision(vscode.Uri.file(plan.output));
  if (!sameRevision(currentOutput, plan.revisions.output)) {
    throw new Error('The output changed after this conversion session loaded. Reload before writing.');
  }

  if (plan.metadata !== undefined) {
    const currentMetadata = await optionalRevision(vscode.Uri.file(plan.metadata));
    if (!sameRevision(currentMetadata, plan.revisions.metadata)) {
      throw new Error('The metadata changed after this conversion session loaded. Reload before writing.');
    }
  }
}

export function rootsOf(found: Discovery): EnfusionRoot[] {
  const roots: EnfusionRoot[] = [];
  for (const mod of found.mods) {
    const anchor = found.uris.get(mod.manifest ?? mod.addons[0]?.config ?? '');
    if (anchor?.scheme !== 'file') {
      continue;
    }
    roots.push({
      root: anchor.with({ path: mod.root }).fsPath,
      ...(mod.prefixRoot === undefined
        ? {}
        : { prefixRoot: anchor.with({ path: mod.prefixRoot }).fsPath }),
    });
  }
  for (const workspacePath of found.workspaces.keys()) {
    const anchor = found.uris.get(workspacePath);
    if (anchor?.scheme === 'file') {
      roots.push({ root: anchor.with({ path: folderOf(workspacePath) }).fsPath });
    }
  }

  return roots.filter(
    (root, at) => roots.findIndex((other) => sameFsPath(other.root, root.root)) === at,
  );
}

function detachedPlan(
  plan: ReadyPlan,
  output: string,
  profile: ReadyPlan['profile'],
): ReadyPlan {
  return {
    ...plan,
    scope: 'detached',
    action: 'convert',
    label: 'Convert',
    output,
    metadata: undefined,
    identity: undefined,
    identityAction: 'none',
    profile,
    revisions: { source: plan.revisions.source },
    notice: undefined,
  };
}

function siblingOutput(source: vscode.Uri): vscode.Uri {
  return vscode.Uri.file(source.fsPath.slice(0, source.fsPath.lastIndexOf('.')) + '.edds');
}

async function metadataOf(
  output: vscode.Uri,
  metadata: vscode.Uri,
  outputRevision: ArtifactRevision | undefined,
  metadataRevision: ArtifactRevision | undefined,
  converter: EddsConverter,
): Promise<ExistingTextureMetadata> {
  if (metadataRevision === undefined) {
    return { kind: 'missing' };
  }
  if (outputRevision === undefined) {
    return {
      kind: 'invalid',
      revision: metadataRevision,
      reason: 'Its sibling EDDS is missing, so the ownership relation cannot be validated.',
    };
  }

  try {
    const inspection = await converter.inspect(output.fsPath, undefined, metadata.fsPath);
    return inspection.metadata === undefined
      ? { kind: 'invalid', revision: metadataRevision, reason: 'Native inspection returned no metadata.' }
      : { kind: 'valid', revision: metadataRevision, value: inspection.metadata };
  } catch (error: unknown) {
    if (error instanceof EddsConverterError && error.category === 'unsupported-format') {
      try {
        const identity = (await converter.inspect(
          output.fsPath,
          undefined,
          metadata.fsPath,
          true,
        )).unsupportedMetadata;
        if (identity !== undefined) {
          return {
            kind: 'unsupported',
            revision: metadataRevision,
            identity: identity.identity,
            reason: identity.reason,
          };
        }
      } catch {
        // The original native refusal remains the authoritative reason below.
      }
    }
    return {
      kind: 'invalid',
      revision: metadataRevision,
      reason: error instanceof Error ? error.message : String(error),
    };
  }
}

async function occupiedGuidsOf(
  roots: readonly EnfusionRoot[],
  converter: EddsConverter,
): Promise<string[]> {
  const metadata = (
    await Promise.all(
      roots.map((root) =>
        vscode.workspace.findFiles(
          new vscode.RelativePattern(root.root, '**/*.edds.meta'),
          '**/{node_modules,.git,dist,out,bin,obj}/**',
        ),
      ),
    )
  ).flat();
  const unique = metadata.filter(
    (uri, at) => metadata.findIndex((candidate) => sameFsPath(candidate.fsPath, uri.fsPath)) === at,
  );

  const identities = await Promise.all(
    unique.map(async (uri) => {
      const output = vscode.Uri.file(uri.fsPath.slice(0, -'.meta'.length));
      try {
        return (await converter.inspect(output.fsPath, undefined, uri.fsPath)).metadata?.guid;
      } catch (error: unknown) {
        if (error instanceof EddsConverterError && error.category === 'unsupported-format') {
          try {
            return (await converter.inspect(output.fsPath, undefined, uri.fsPath, true))
              .unsupportedMetadata?.identity.guid;
          } catch {
            return undefined;
          }
        }
        return undefined;
      }
    }),
  );
  return identities.filter((guid): guid is string => guid !== undefined);
}

async function requiredRevision(uri: vscode.Uri, message: string): Promise<ArtifactRevision> {
  try {
    const stat = await vscode.workspace.fs.stat(uri);
    if (stat.type !== vscode.FileType.File) {
      throw new Error(message);
    }
    return { size: stat.size, modified: stat.mtime };
  } catch {
    throw new Error(message);
  }
}

async function optionalRevision(uri: vscode.Uri): Promise<ArtifactRevision | undefined> {
  try {
    const stat = await vscode.workspace.fs.stat(uri);
    return stat.type === vscode.FileType.File ? { size: stat.size, modified: stat.mtime } : undefined;
  } catch {
    return undefined;
  }
}

function sameRevision(
  current: ArtifactRevision | undefined,
  expected: ArtifactRevision | undefined,
): boolean {
  return (
    current === expected ||
    (current?.size === expected?.size && current?.modified === expected?.modified)
  );
}

function sameFsPath(left: string, right: string): boolean {
  return path.resolve(left).toLowerCase() === path.resolve(right).toLowerCase();
}

function sameUri(left: vscode.Uri, right: vscode.Uri): boolean {
  return left.scheme === right.scheme &&
    (left.scheme === 'file' ? sameFsPath(left.fsPath, right.fsPath) : left.toString() === right.toString());
}
