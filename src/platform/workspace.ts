/**
 * The workspace side of mod discovery: the search, the watcher, and the `Uri` that goes with each
 * mod. `src/mods/` never sees any of it, which is what keeps its tests on plain Node.
 */

import * as vscode from 'vscode';
import { CONFIG_FILE, type Mod, modsFromConfigs } from '../mods/scan';

/** Everything named `config.cpp`, anywhere in the open folders. */
const CONFIG_GLOB = `**/${CONFIG_FILE}`;

/** Folders that never hold a mod but do hold thousands of files. */
const EXCLUDE_GLOB = '**/{node_modules,.git,dist,out,bin,obj}/**';

/** A mod together with the `Uri` it was found through, so opening it needs no path arithmetic. */
export interface ModEntry {
  readonly mod: Mod;
  readonly config: vscode.Uri;
}

/** Every mod of the open folders. Honours the user's `files.exclude` and `search.exclude`. */
export async function findMods(): Promise<ModEntry[]> {
  const found = await vscode.workspace.findFiles(CONFIG_GLOB, EXCLUDE_GLOB);
  const uris = new Map(found.map((uri) => [uri.path, uri] as const));

  return modsFromConfigs([...uris.keys()]).flatMap((mod) => {
    const config = uris.get(mod.config);
    return config ? [{ mod, config }] : [];
  });
}

/**
 * Calls back whenever the set of mods can have changed: a `config.cpp` appearing or going away,
 * and a folder being added to or removed from the workspace. Edits inside the file are not a
 * change of the set, so the watcher ignores them.
 */
export function watchMods(onChange: () => void): vscode.Disposable {
  const watcher = vscode.workspace.createFileSystemWatcher(CONFIG_GLOB, false, true, false);

  return vscode.Disposable.from(
    watcher,
    watcher.onDidCreate(onChange),
    watcher.onDidDelete(onChange),
    vscode.workspace.onDidChangeWorkspaceFolders(onChange),
  );
}
