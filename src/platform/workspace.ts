/**
 * The workspace side of mod discovery: the search, the reads, the watcher, and the `Uri` that goes
 * with each path. `src/mods/` never sees any of it, which is what keeps its tests on plain Node.
 */

import * as vscode from 'vscode';
import {
  type Configured,
  type Launch,
  type ManifestProblem,
  type ManifestSource,
  type ModLocation,
  NO_LAUNCH,
  WORKSPACE_FILE,
  configurationsOf,
  workspaceFor,
} from '../mods/enf';
import type { LaunchMod, TargetSource } from '../mods/launch';
import { CONFIG_FILE, MANIFEST_FILE, type Mod, modsFromScan, pboNameOf } from '../mods/model';
import { folderOf, nameOf, windowsFolder } from '../mods/paths';
import type { Prefix } from '../mods/workDrive';

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

  // The manifests are read before the mods are worked out, because the name in one is what the mod
  // is called — and which file configures which mod needs no more than where each of them sits,
  // which is the folder its `mod.enf` is in. Which is the domain's call all the same, not this
  // module's.
  const configurations = configurationsOf(locationsOf(manifests), enf);
  const mods = modsFromScan({ manifests, configs, declared: declaredOf(configurations.mods) });

  return { mods, uris, configured: configurations.mods, workspaces: configurations.workspaces };
}

/** All the cascade asks about a mod: where it sits, and which file configures it. */
function locationsOf(manifests: readonly string[]): ModLocation[] {
  return manifests.map((manifest) => ({ root: folderOf(manifest), manifest }));
}

/** The name each manifest declares, for the mods to be named by; one that declares none is absent. */
function declaredOf(configured: ReadonlyMap<string, Configured>): Map<string, string> {
  return new Map(
    [...configured].flatMap(([path, mod]) => {
      const name = mod.configuration.manifest.name;

      return name === undefined || name === '' ? [] : [[path, name] as const];
    }),
  );
}

/**
 * The mods that have something to put on the work drive, with the prefix root as a path on disk.
 *
 * The folder has no `Uri` of its own — only files were searched for — so it borrows one from a
 * file inside the mod and swaps the path. Building a `Uri` from the path instead would assume the
 * workspace is on this disk, and `fsPath` is the one thing that turns it into what Windows takes.
 */
export function prefixesOf(found: Discovery): Prefix[] {
  return found.mods.flatMap((mod) => {
    const prefixRoot = mod.prefixRoot;
    const anchor = found.uris.get(mod.manifest ?? mod.addons[0]?.config ?? '');

    if (prefixRoot === undefined || anchor === undefined) {
      return [];
    }

    return [{ prefixRoot, name: mod.name, target: anchor.with({ path: prefixRoot }).fsPath }];
  });
}

/**
 * The mods a launch loads, with the folders it takes them out of as paths on disk: the prefix root
 * it links, and the mod root the profile and the mission are laid down from. The same borrowing of
 * a `Uri` as `prefixesOf`, and for the same reason.
 */
export function launchModsOf(found: Discovery): LaunchMod[] {
  return found.mods.flatMap((mod) => {
    const prefixRoot = mod.prefixRoot;
    const anchor = found.uris.get(mod.manifest ?? mod.addons[0]?.config ?? '');

    if (prefixRoot === undefined || anchor === undefined) {
      return [];
    }

    return [
      {
        name: mod.name,
        root: anchor.with({ path: mod.root }).fsPath,
        prefixRoot: anchor.with({ path: prefixRoot }).fsPath,
        // By the name each of them packs into rather than by its folder's, because what this is
        // for is looking the pbo up in the built mod before the game is told to load it.
        addons: mod.addons.map((addon) => pboNameOf(mod, addon)),
      },
    ];
  });
}

/**
 * A mod with the `.enf` that configures it resolved: which file owns its launch block, where that
 * file sits, and what it says. The cascade has already picked the file — a `workspace.enf` owns
 * the launch of everything under it — and a relative path is counted from that file's folder,
 * which is the one place a path in a manifest means what it says.
 */
export interface Owned {
  readonly mod: Mod;
  /** The file whose launch block this mod obeys; empty for a mod with no manifest at all. */
  readonly owner: string;
  /** Its name — `mod.enf` or `workspace.enf` — for the sentence that asks for a setting. */
  readonly configuredBy: string;
  /** Its folder, the way Windows takes it. */
  readonly configuredIn: string;
  readonly launch: Launch;
  /** What the mod's own manifest excludes from packing, which no workspace file overrides. */
  readonly exclude: readonly string[];
}

/** Every mod of the workspace, in the model's order, with the file that configures each. */
export function ownedOf(found: Discovery): Owned[] {
  return found.mods.map((mod) => {
    const configured = mod.manifest === undefined ? undefined : found.configured.get(mod.manifest);
    const owner = configured?.workspace ?? mod.manifest;
    const ownerUri = owner === undefined ? undefined : found.uris.get(owner);

    return {
      mod,
      owner: owner ?? '',
      configuredBy: owner === undefined ? MANIFEST_FILE : nameOf(owner),
      configuredIn:
        ownerUri === undefined ? rootOf(mod.root, found) : windowsFolder(ownerUri.fsPath),
      launch: configured?.configuration.launch ?? NO_LAUNCH,
      exclude: configured?.configuration.manifest.exclude ?? [],
    };
  });
}

/**
 * The launch blocks of the workspace, one per mod, for the targets to be read out of. Both the
 * launcher and the panel ask for these — one to put the game up, the other to know whether there
 * is anything to put up — and a second way of gathering them would be a second answer.
 */
export function targetSourcesOf(found: Discovery): TargetSource[] {
  return ownedOf(found).map((owned) => ({
    mod: owned.mod.name,
    owner: owned.owner,
    configuredBy: owned.configuredBy,
    configuredIn: owned.configuredIn,
    launch: owned.launch,
  }));
}

/**
 * The mod root as Windows takes it. The folder has no `Uri` of its own — only files were searched
 * for — so it borrows one from a file inside the mod and swaps the path, which is what keeps this
 * working for a workspace that is not on this disk.
 */
function rootOf(root: string, found: Discovery): string {
  const anchor = [...found.uris.values()].find((uri) => uri.path.startsWith(`${root}/`));

  return anchor ? anchor.with({ path: root }).fsPath : '';
}

/**
 * The `workspace.enf` that owns a mod's launch block, named the way it is shown, or undefined
 * where the mod owns its own. Only the workspace files are searched for: a form over one manifest
 * has no business scanning the whole workspace for mods to answer one question about itself.
 */
export async function launchOwnerOf(modRoot: string): Promise<string | undefined> {
  const found = await vscode.workspace.findFiles(`**/${WORKSPACE_FILE}`, EXCLUDE_GLOB);
  // Which file owns which mod is the domain's call here as much as it is in `findMods`.
  const owner = workspaceFor(
    modRoot,
    found.map((uri) => uri.path),
  );
  const uri = found.find((candidate) => candidate.path === owner);

  return uri === undefined ? undefined : vscode.workspace.asRelativePath(uri, true);
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
