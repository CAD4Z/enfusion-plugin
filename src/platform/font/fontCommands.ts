/** Captures write targets before confirmation, and checks them again just before invoking native code. */
import { createHash } from 'node:crypto';
import path from 'node:path';
import * as vscode from 'vscode';
import {
  fontCommandPlanOf, fontCommandRefusalOf, fontTargetsOf, type FontCommandInput, type FontCommandPlan, type FontRequest,
} from '../../mods/font/fontCommands';
import { rootsOf } from '../texture/textureConversion';
import { findMods } from '../workspace';

interface FontArtifactRevision {
  readonly fingerprint: string;
  readonly size: number;
  readonly modified: number;
}

export interface FontCommandSession {
  readonly plan: Extract<FontCommandPlan, { kind: 'ready' }>;
  readonly input: FontCommandInput;
  readonly revisions: ReadonlyMap<string, FontArtifactRevision | undefined>;
}

export async function assertFontScope(uri: vscode.Uri, kind: FontRequest['kind']): Promise<void> {
  const refusal = fontCommandRefusalOf({ kind, file: uri.fsPath, scheme: uri.scheme,
    platform: process.platform, roots: rootsOf(await findMods()) });
  if (refusal !== undefined) throw new Error(refusal);
  if ((await vscode.workspace.fs.stat(uri)).type !== vscode.FileType.File) throw new Error('Choose a font file, not a folder.');
}

export async function loadFontCommand(uri: vscode.Uri, request: FontRequest): Promise<FontCommandSession> {
  await assertFontScope(uri, request.kind);
  const targets = fontTargetsOf(uri.fsPath, request);
  const files = [targets.output, targets.atlas, targets.metadata];
  const revisions = new Map<string, FontArtifactRevision | undefined>();
  for (const file of files) revisions.set(file, await revisionOf(file));
  const input: FontCommandInput = {
    ...request, file: uri.fsPath, scheme: uri.scheme, platform: process.platform,
    roots: rootsOf(await findMods()), existing: files.filter((file) => revisions.get(file) !== undefined),
    metadata: { kind: revisions.get(targets.metadata) === undefined ? 'missing' : 'present' },
  };
  const plan = fontCommandPlanOf(input);
  if (plan.kind === 'refused') throw new Error(plan.reason);
  return { plan, input, revisions };
}

export async function assertFontCommandCurrent(session: FontCommandSession): Promise<void> {
  const plan = fontCommandPlanOf({ ...session.input, roots: rootsOf(await findMods()) });
  if (plan.kind === 'refused') throw new Error(plan.reason);
  if (plan.resourceName !== session.plan.resourceName) throw new Error('The Enfusion roots changed. Run the font command again.');
  for (const [file, revision] of session.revisions) {
    if ((await revisionOf(file))?.fingerprint !== revision?.fingerprint) {
      throw new Error(`${path.basename(file)} changed. Run the font command again before replacing it.`);
    }
  }
}

async function revisionOf(file: string): Promise<FontArtifactRevision | undefined> {
  const uri = vscode.Uri.file(file);
  try {
    const stat = await vscode.workspace.fs.stat(uri);
    if (stat.type !== vscode.FileType.File) throw new Error(`${path.basename(file)} is not a regular file.`);
    return { fingerprint: createHash('sha256').update(await vscode.workspace.fs.readFile(uri)).digest('hex'),
      size: stat.size, modified: stat.mtime };
  } catch (error: unknown) {
    if (error instanceof vscode.FileSystemError && error.code === 'FileNotFound') return undefined;
    throw error;
  }
}
