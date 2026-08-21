/**
 * The workspace side of mod discovery: the search, the reads, the watcher, and the `Uri` that goes
 * with each path. `src/mods/` never sees any of it, which is what keeps its tests on plain Node.
 */

import * as vscode from 'vscode';
import { CONFIG_FILE, MANIFEST_FILE, type Mod, modsFromScan } from '../mods/model';

/** The two files a mod is made of, anywhere in the open folders. */
const SCAN_GLOB = `**/{${MANIFEST_FILE},${CONFIG_FILE}}`;

/** Folders that never hold a mod but do hold thousands of files. */
const EXCLUDE_GLOB = '**/{node_modules,.git,dist,out,bin,obj}/**';

/** The mods of the workspace, with the `Uri` each of their paths was found through. */
export interface Discovery {
  readonly mods: readonly Mod[];
  /** Opening a file means reaching for the `Uri` here, never rebuilding one from the path. */
  readonly uris: ReadonlyMap<string, vscode.Uri>;
}

/** Every mod of the open folders. Honours the user's `files.exclude` and `search.exclude`. */
export async function findMods(): Promise<Discovery> {
  const found = await vscode.workspace.findFiles(SCAN_GLOB, EXCLUDE_GLOB);
  const uris = new Map(found.map((uri) => [uri.path, uri] as const));

  const manifests = found.filter(named(MANIFEST_FILE)).map((uri) => uri.path);
  const configs = await Promise.all(
    found.filter(named(CONFIG_FILE)).map(async (uri) => ({
      path: uri.path,
      source: await read(uri),
    })),
  );

  return { mods: modsFromScan({ manifests, configs }), uris };
}

/**
 * Calls back whenever the mods can have changed: either file appearing or going away, a
 * `config.cpp` being edited — the addons it declares and what they require are read out of it —
 * and a folder joining or leaving the workspace.
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

function named(file: string): (uri: vscode.Uri) => boolean {
  return (uri) => uri.path.endsWith(`/${file}`);
}

/**
 * A file that vanished between the search and the read is not worth failing the whole scan over:
 * an unreadable `config.cpp` declares no addon, which is what an empty one parses to anyway.
 */
async function read(uri: vscode.Uri): Promise<string> {
  try {
    return new TextDecoder().decode(await vscode.workspace.fs.readFile(uri));
  } catch {
    return '';
  }
}
