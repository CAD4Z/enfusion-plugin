/**
 * The mods of a workspace, worked out from what a search found.
 *
 * A mod is a folder with `mod.enf`. Inside it sits the prefix root — the folder that gets linked
 * onto the work drive — and inside that, the addons: folders with a `config.cpp`, one pbo each.
 *
 * The layout is read off the tree rather than declared: a `config.cpp` in the prefix root itself
 * means the whole mod packs into one pbo, and its absence means the addons are the subfolders.
 * Which folder is the prefix root comes from the main addon — the one whose `config.cpp` carries
 * `CfgMods` — and from the name the manifest declares, both of which name it.
 *
 * What the mod is *called* is the manifest's to say and nobody else's: one name, which the panel
 * shows, `P:\<name>` goes up as and `@<name>` is built into. Only a mod that declares none falls
 * back to the name of its folder.
 *
 * Paths are `/` separated throughout, which is what `Uri.path` hands over on every platform.
 */

import { parseConfig, sameName } from './config';
import { folderOf, isWithin, nameOf } from './paths';

/** What a search of the workspace turned up. */
export interface Scan {
  /** Paths of every `mod.enf`. Their folders are the mod roots. */
  readonly manifests: readonly string[];
  /** Every `config.cpp` with its text, which is where the addons come from. */
  readonly configs: readonly ConfigFile[];
  /**
   * The name each `mod.enf` declares, by the path of that file. It is the one name a mod has —
   * what the panel shows, what `P:\<name>` goes up as, what the built `@<name>` is called — so a
   * mod whose folder is called something else says so there rather than being guessed at. A mod
   * that declares none is simply not in the map, and falls back to its folder's name.
   */
  readonly declared?: ReadonlyMap<string, string>;
}

export interface ConfigFile {
  readonly path: string;
  readonly source: string;
}

/** How the addons sit in the prefix root: one pbo for the whole mod, or one per subfolder. */
export type Layout = 'single' | 'multi';

/** One mod of the workspace. */
export interface Mod {
  /**
   * The mod's one name: what the panel shows, and what it is linked (`P:\<name>`) and loaded
   * (`@<name>`) as. Its `mod.enf` declares it; failing that, it is the folder's own name.
   */
  readonly name: string;
  /** The mod root: the folder holding `mod.enf`, or the prefix root's parent without one. */
  readonly root: string;
  /** The `mod.enf` itself; undefined leaves the mod unconfigured. */
  readonly manifest: string | undefined;
  /** The folder linked onto the work drive; undefined when no addon was found to name it. */
  readonly prefixRoot: string | undefined;
  /** Undefined together with the prefix root: with no addon there is no layout to read. */
  readonly layout: Layout | undefined;
  /** In build order. */
  readonly addons: readonly Addon[];
  readonly problems: readonly Problem[];
}

/** One addon: a folder with a `config.cpp`, packed into a pbo named after the folder. */
export interface Addon {
  /** Folder name, which is also the pbo's name — unrelated to the `CfgPatches` class names. */
  readonly name: string;
  readonly root: string;
  readonly config: string;
  /** Carries `CfgMods`, so it is this mod's main addon. */
  readonly main: boolean;
  /** The `CfgPatches` classes it declares: the names other addons require it by. */
  readonly patches: readonly string[];
  readonly requires: readonly string[];
  /** Required addons no addon in the workspace declares — vanilla ones, or a typo. */
  readonly unresolved: readonly string[];
}

/** Something worth showing next to the mod rather than hiding or throwing over. */
export type Problem =
  /** Nothing under the mod root packs into a pbo. */
  | { readonly kind: 'no-addons' }
  /** The `CfgPatches` names that require each other in a ring, so no build order can hold. */
  | { readonly kind: 'cycle'; readonly patches: readonly string[] };

/** The mods of the scan, in build order. */
export function modsFromScan(scan: Scan): Mod[] {
  const sources = scan.configs.map(read);
  const manifests = new Map(scan.manifests.map((path) => [folderOf(path), path] as const));
  const roots = [...manifests.keys()];

  // Sorted before the graph gets a say, so that mods and addons it does not relate — and a search
  // hands them over in whatever order it likes — still come out the same way every scan.
  const drafts = [
    ...[...manifests].map(([root, manifest]) =>
      draftAt(root, manifest, scan.declared?.get(manifest), sources),
    ),
    ...unconfigured(sources.filter((source) => !roots.some((root) => isWithin(source.root, root)))),
  ].sort(byName(modName));

  const addons = drafts.flatMap((draft) => draft.addons).sort(byName((addon) => addon.name));
  const provider = providerOf(addons);
  const owner = new Map(drafts.flatMap((draft) => draft.addons.map((addon) => [addon, draft])));

  // Ordering the addons of the whole workspace at once is what lets a mod be ordered by the
  // addons of another one: both orders are read off the same graph.
  const walk = ordered(addons, (addon) => requiredBy(addon, provider));
  const rank = rankOf(walk.order);

  // A ring between mods is a ring between their addons, so the addon walk has already found it.
  const mods = ordered(drafts, (draft) =>
    draft.addons
      .flatMap((addon) => requiredBy(addon, provider))
      .flatMap((required) => {
        const other = owner.get(required);
        return other && other !== draft ? [other] : [];
      }),
  ).order;

  return mods.map((draft) => toMod(draft, provider, rank, walk.cycles));
}

/**
 * The addon that declares the mod: the one carrying `CfgMods`. A mod has one, and a mod whose
 * declaration sits deeper than an addon has none — which is the one thing everything reading a
 * mod's own name, author or script modules out of a config has to ask first.
 */
export function mainAddonOf(mod: Mod): Addon | undefined {
  return mod.addons.find((addon) => addon.main);
}

/**
 * The pbo an addon packs into, which is not always its folder's name.
 *
 * A builder names the pbo after the last folder of the path it was pointed at, and that path runs
 * through the work drive: `P:\<Mod>\<Addon>` for an addon inside the prefix root, and `P:\<Mod>`
 * for the addon that *is* the prefix root — because the prefix root goes onto the drive under the
 * mod's name rather than under its own. So a mod in a folder called `client` that calls itself
 * `CADNavigationClient` packs into `CADNavigationClient.pbo`.
 */
export function pboNameOf(mod: Mod, addon: Addon): string {
  return addon.root === mod.prefixRoot ? mod.name : addon.name;
}

/** The file whose presence declares a folder to be a mod. */
export const MANIFEST_FILE = 'mod.enf';

/** The file whose presence declares a folder to be an addon. */
export const CONFIG_FILE = 'config.cpp';

/** A mod before its addons have been put in build order. */
interface Draft {
  readonly root: string;
  readonly manifest: string | undefined;
  /** What its `mod.enf` calls it; undefined where that file says nothing, or where there is none. */
  readonly declared: string | undefined;
  readonly prefixRoot: string | undefined;
  readonly layout: Layout | undefined;
  readonly addons: readonly AddonSource[];
}

function draftAt(
  root: string,
  manifest: string,
  declared: string | undefined,
  sources: readonly AddonSource[],
): Draft {
  const under = sources.filter((source) => isWithin(source.root, root));
  const main = under.find((source) => source.main);
  const prefixRoot = main && prefixRootFrom(main, root, declared);

  return { root, manifest, declared, prefixRoot, ...addonsOf(prefixRoot, under) };
}

/**
 * Mods among the leftovers: what declares a mod is the `CfgMods` block, so a `config.cpp` without
 * one is an addon of something — a mission, a folder inside a mod — and not a mod of its own.
 * Anything else the workspace holds never reaches the list at all.
 */
function unconfigured(sources: readonly AddonSource[]): Draft[] {
  const roots = new Map<string, Draft>();

  for (const main of sources.filter((source) => source.main)) {
    // With no `mod.enf` to bound it, the walk up stops wherever the paths do.
    const prefixRoot = prefixRootFrom(main, '', undefined);
    if (roots.has(prefixRoot)) {
      continue;
    }

    const under = sources.filter((source) => isWithin(source.root, prefixRoot));
    roots.set(prefixRoot, {
      root: folderOf(prefixRoot),
      manifest: undefined,
      declared: undefined,
      prefixRoot,
      ...addonsOf(prefixRoot, under),
    });
  }

  return [...roots.values()];
}

/** Two of the same name — a mod next to a copy of it — keep a stable order by where they sit. */
function byName<T extends { readonly root: string }>(name: (item: T) => string) {
  return (a: T, b: T) => name(a).localeCompare(name(b)) || a.root.localeCompare(b.root);
}

/**
 * The mod's name: the one its `mod.enf` declares, and failing that its folder's own — the prefix
 * root's, and the mod root's only until one is found. A mod whose folder goes by something other
 * than the mod does — `client` holding `CADNavigationClient` — is what declaring one is for.
 */
function modName(draft: Draft): string {
  return draft.declared ?? nameOf(draft.prefixRoot ?? draft.root);
}

function toMod(
  draft: Draft,
  provider: ReadonlyMap<string, AddonSource>,
  rank: Ranking,
  cycles: readonly (readonly AddonSource[])[],
): Mod {
  const addons = [...draft.addons].sort((a, b) => rank(a) - rank(b));

  return {
    name: modName(draft),
    root: draft.root,
    manifest: draft.manifest,
    prefixRoot: draft.prefixRoot,
    layout: draft.layout,
    addons: addons.map((addon) => toAddon(addon, provider)),
    problems: problemsOf(draft, cycles),
  };
}

function problemsOf(draft: Draft, cycles: readonly (readonly AddonSource[])[]): Problem[] {
  if (draft.addons.length === 0) {
    return [{ kind: 'no-addons' }];
  }

  return cycles
    .filter((cycle) => cycle.some((member) => draft.addons.includes(member)))
    .map((cycle) => ({ kind: 'cycle', patches: unique(cycle.map(addonName)).sort() }));
}

/** How an addon appears in someone else's `requiredAddons`; the folder name is the last resort. */
function addonName(source: AddonSource): string {
  return source.patches[0] ?? source.name;
}

/** A `config.cpp` reduced to what the model reads from it. */
interface AddonSource {
  readonly config: string;
  readonly root: string;
  readonly name: string;
  readonly patches: readonly string[];
  readonly requires: readonly string[];
  readonly main: boolean;
  /** `dir` of `CfgMods`: the prefix root's name, straight from the addon that declares the mod. */
  readonly dir: string;
}

function read(file: ConfigFile): AddonSource {
  const parsed = parseConfig(file.source);
  const root = folderOf(file.path);

  return {
    config: file.path,
    root,
    name: nameOf(root),
    patches: parsed.patches.map((patch) => patch.name),
    requires: unique(parsed.patches.flatMap((patch) => patch.requiredAddons)),
    main: parsed.mod !== undefined,
    dir: parsed.mod?.dir ?? '',
  };
}

function toAddon(source: AddonSource, provider: ReadonlyMap<string, AddonSource>): Addon {
  return {
    name: source.name,
    root: source.root,
    config: source.config,
    main: source.main,
    patches: source.patches,
    requires: source.requires,
    unresolved: source.requires.filter((required) => !provider.has(lower(required))),
  };
}

/** Which addon answers a name in `requiredAddons`. The first to claim a name keeps it. */
function providerOf(addons: readonly AddonSource[]): Map<string, AddonSource> {
  const provider = new Map<string, AddonSource>();

  for (const addon of addons) {
    for (const patch of addon.patches) {
      if (!provider.has(lower(patch))) {
        provider.set(lower(patch), addon);
      }
    }
  }

  return provider;
}

/** The addons this one has to be built after. Vanilla and mistyped names simply have no answer. */
function requiredBy(addon: AddonSource, provider: ReadonlyMap<string, AddonSource>): AddonSource[] {
  return addon.requires.flatMap((required) => {
    const found = provider.get(lower(required));
    return found && found !== addon ? [found] : [];
  });
}

/** Where a node ended up in an order, for sorting a subset of it. */
type Ranking = (node: AddonSource) => number;

function rankOf(order: readonly AddonSource[]): Ranking {
  const ranks = new Map(order.map((node, index) => [node, index] as const));
  return (node) => ranks.get(node) ?? 0;
}

/**
 * Dependencies first, and otherwise the order they came in, so the result only differs from the
 * input where the graph forces it. A ring stops the walk instead of hanging it: its nodes still
 * come out, in an order nothing can justify, and come back as a cycle for the caller to report.
 */
function ordered<T>(
  nodes: readonly T[],
  requires: (node: T) => readonly T[],
): { order: T[]; cycles: T[][] } {
  const order: T[] = [];
  const cycles: T[][] = [];
  const done = new Set<T>();
  const walking: T[] = [];

  const visit = (node: T): void => {
    if (done.has(node)) {
      return;
    }

    const ring = walking.indexOf(node);
    if (ring !== -1) {
      cycles.push(walking.slice(ring));
      return;
    }

    walking.push(node);
    for (const required of requires(node)) {
      visit(required);
    }
    walking.pop();

    done.add(node);
    order.push(node);
  };

  nodes.forEach(visit);
  return { order, cycles };
}

/**
 * The main addon names the prefix root through `dir`, so the prefix root is the nearest folder up
 * from it that goes by that name — itself in a single-addon mod, its parent in a multi-addon one.
 * The name the manifest declares answers for it just as well: the two are the same name, and a mod
 * that has written its down should not have to wait for its config to agree before it is found.
 * The walk stops at `bound`, which is the mod root when there is one.
 */
function prefixRootFrom(main: AddonSource, bound: string, declared: string | undefined): string {
  const names = [main.dir, declared ?? ''].filter((name) => name !== '');

  for (let folder = main.root; folder !== '' && isWithin(folder, bound); folder = folderOf(folder)) {
    if (names.some((name) => sameName(nameOf(folder), name))) {
      return folder;
    }
  }

  // The mod is linked under the folder name regardless of what `dir` claims, so the addon's own
  // folder is the best answer left.
  return main.root;
}

/** A `config.cpp` in the prefix root itself packs the whole mod; otherwise the subfolders do. */
function addonsOf(
  prefixRoot: string | undefined,
  sources: readonly AddonSource[],
): { layout: Layout | undefined; addons: readonly AddonSource[] } {
  if (prefixRoot === undefined) {
    return { layout: undefined, addons: [] };
  }

  const whole = sources.find((source) => source.root === prefixRoot);
  if (whole) {
    return { layout: 'single', addons: [whole] };
  }

  // Only the immediate subfolders: a `config.cpp` deeper than that is packed by the addon above it.
  return { layout: 'multi', addons: sources.filter((source) => folderOf(source.root) === prefixRoot) };
}

function unique(values: readonly string[]): string[] {
  return [...new Set(values)];
}

function lower(value: string): string {
  return value.toLowerCase();
}

