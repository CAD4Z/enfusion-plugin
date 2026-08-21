/**
 * The workspace side of mod discovery: the search, the reads, the watcher, and the `Uri` that goes
 * with each path. `src/mods/` never sees any of it, which is what keeps its tests on plain Node.
 */

import * as vscode from 'vscode';
import {
  type Configured,
  type ManifestProblem,
  type ManifestSource,
  WORKSPACE_FILE,
  configurationsOf,
} from '../mods/enf';
import { CONFIG_FILE, MANIFEST_FILE, type Mod, modsFromScan } from '../mods/model';
import { nameOf } from '../mods/paths';

/** The three files a workspace of mods is made of, anywhere in the open folders. */
const SCAN_GLOB = `**/{${MANIFEST_FILE},${WORKSPACE_FILE},${CONFIG_FILE}}`;

/** Folders that never hold a mod but do hold thousands of files. */
const EXCLUDE_GLOB = '**/{node_modules,.git,dist,out,bin,obj}/**';

/** The mods of the workspace, with the `Uri` each of their paths was found through. */
export interface Discovery {
  readonly mods: readonly Mod[];
  /** Opening a file means reaching for the `Uri` here, never rebuilding one from the path. */
  readonly uris: ReadonlyMap<string, vscode.Uri>;
  /** What each mod is configured to be, by the path of its `mod.enf`. */
  readonly configured: ReadonlyMap<string, Configured>;
  /** Every `workspace.enf` of the open folders, by path, with what is wrong with it. */
  readonly workspaces: ReadonlyMap<string, readonly ManifestProblem[]>;
}

/** Every mod of the open folders. Honours the user's `files.exclude` and `search.exclude`. */
export async function findMods(): Promise<Discovery> {
  const found = await vscode.workspace.findFiles(SCAN_GLOB, EXCLUDE_GLOB);
  const uris = new Map(found.map((uri) => [uri.path, uri] as const));

  const [enf, configs] = await Promise.all([
    sources(found.filter(named(MANIFEST_FILE, WORKSPACE_FILE))),
    sources(found.filter(named(CONFIG_FILE))),
  ]);

  const manifests = enf
    .filter((file) => nameOf(file.path) === MANIFEST_FILE)
    .map((file) => file.path);
  const mods = modsFromScan({ manifests, configs });

  // Which file configures which mod is the domain's call, not this module's.
  const configurations = configurationsOf(mods, enf);

  return { mods, uris, configured: configurations.mods, workspaces: configurations.workspaces };
}

/**
 * Calls back whenever the mods can have changed: either file appearing or going away, a
 * `config.cpp` being edited — the addons it declares and what they require are read out of it —
 * a `.enf` being edited, and a folder joining or leaving the workspace.
 */
export function watchMods(onChange: () => void): vscode.Disposable {
  const watcher = vscode.workspace.createFileSystemWatcher(SCAN_GLOB);

  return vscode.Disposable.from(
    watcher,
    watcher.onDidCreate(onChange),
    watcher.onDidChange(onChange),
    watcher.onDidDelete(onChange),
    vscode.workspace.onDidChangeWorkspaceFolders(onChange),
  );
}

/** A `Uri` carries no basename of its own, so the name is read off the end of the path. */
function named(...files: readonly string[]): (uri: vscode.Uri) => boolean {
  return (uri) => files.some((file) => nameOf(uri.path) === file);
}

async function sources(uris: readonly vscode.Uri[]): Promise<ManifestSource[]> {
  return Promise.all(uris.map(async (uri) => ({ path: uri.path, source: await text(uri) })));
}

/**
 * A file that vanished between the search and the read is not worth failing the whole scan over:
 * an unreadable `config.cpp` declares no addon, which is what an empty one parses to anyway.
 */
async function text(uri: vscode.Uri): Promise<string> {
  try {
    return new TextDecoder().decode(await vscode.workspace.fs.readFile(uri));
  } catch {
    return '';
  }
}
