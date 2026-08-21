/**
 * Carrying out an initialisation plan. What a new mod is made of was decided in
 * `src/mods/init.ts`; this makes the folders and writes the files, and refuses to write over
 * anything that is there already.
 *
 * Through `vscode.workspace.fs` rather than through Node: what is being written is the open
 * workspace, and the workspace is not always a folder on this disk.
 */

import * as vscode from 'vscode';
import { type AddonRequirement, type InitPlan, requiringAddon } from '../mods/init';

/** The files of the plan that are on disk already: somebody's work, and not ours to write over. */
export async function existingOf(root: vscode.Uri, plan: InitPlan): Promise<string[]> {
  const found = await Promise.all(
    plan.files.map(async (file) => ((await exists(uriOf(root, file.path))) ? [file.path] : [])),
  );

  return found.flat();
}

/**
 * The plan, carried out under the folder given: the folders first, so the empty ones are made as
 * well, and then the files. A file writes its own folders, so the two lists need not agree.
 */
export async function createFrom(root: vscode.Uri, plan: InitPlan): Promise<void> {
  for (const folder of plan.folders) {
    await vscode.workspace.fs.createDirectory(uriOf(root, folder));
  }

  for (const file of plan.files) {
    const uri = uriOf(root, file.path);
    await vscode.workspace.fs.createDirectory(parentOf(uri));
    await vscode.workspace.fs.writeFile(uri, new TextEncoder().encode(file.content));
  }
}

/**
 * The new addon written into the main addon's `requiredAddons`. False where the class it was to
 * be written into is no longer in the file — which is a thing to report rather than to throw over,
 * because by then the addon itself has been made.
 */
export async function requireAddon(
  config: vscode.Uri,
  requirement: AddonRequirement,
): Promise<boolean> {
  const source = new TextDecoder().decode(await vscode.workspace.fs.readFile(config));
  const written = requiringAddon(source, requirement);

  if (written === undefined) {
    return false;
  }

  await vscode.workspace.fs.writeFile(config, new TextEncoder().encode(written));
  return true;
}

/**
 * What a file holds, and nothing at all where it holds nothing readable: a `config.cpp` that went
 * away between the scan and the read declares no mod, which is what an empty one parses to anyway.
 * `requireAddon` reads for itself rather than through this, because a read it swallowed would have
 * it write an edited file over one it never saw.
 */
export async function textOf(uri: vscode.Uri): Promise<string> {
  try {
    return new TextDecoder().decode(await vscode.workspace.fs.readFile(uri));
  } catch {
    return '';
  }
}

/** Whether a folder holds a file of that name, which is how a mod root is recognised. */
export async function holds(folder: vscode.Uri, name: string): Promise<boolean> {
  return exists(vscode.Uri.joinPath(folder, name));
}

/** The folder a path names, which is the path itself unless it names a file. */
export async function folderAt(uri: vscode.Uri): Promise<vscode.Uri> {
  try {
    const stat = await vscode.workspace.fs.stat(uri);
    return stat.type === vscode.FileType.File ? parentOf(uri) : uri;
  } catch {
    // Nothing there to ask about, so it is taken for the folder it looks like.
    return uri;
  }
}

/** A path of the plan under the root it is counted from; the plan writes `/` either way. */
function uriOf(root: vscode.Uri, path: string): vscode.Uri {
  return vscode.Uri.joinPath(root, ...path.split('/'));
}

function parentOf(uri: vscode.Uri): vscode.Uri {
  return vscode.Uri.joinPath(uri, '..');
}

async function exists(uri: vscode.Uri): Promise<boolean> {
  try {
    await vscode.workspace.fs.stat(uri);
    return true;
  } catch {
    return false;
  }
}
