/**
 * Finding mods in a set of paths.
 *
 * A mod is a folder with a `config.cpp` in its root — the same rule the Workbench build follows,
 * where that one file is what makes the folder pack into a single pbo. The mod's name is the name
 * of that folder, because that is what the work drive links it as (`P:\<Mod>`) and what the game
 * loads it as (`@<Mod>`); `CfgPatches` is not read here.
 *
 * Paths are `/` separated throughout, which is what `Uri.path` hands over on every platform.
 */

/** One mod of the open workspace. */
export interface Mod {
  /** Folder name of the mod root: `CADCore` for `/f:/Code/cad4z/CADCore/CADCore/config.cpp`. */
  readonly name: string;
  /** The folder holding `config.cpp`. */
  readonly root: string;
  /** The `config.cpp` itself, which is also this mod's identity. */
  readonly config: string;
}

/** The file whose presence declares a folder to be a mod. */
export const CONFIG_FILE = 'config.cpp';

/**
 * Mods of the given `config.cpp` paths, one per folder, sorted by name.
 *
 * Two paths pointing at the same folder collapse into one mod, so the caller is free to hand over
 * whatever a search returned without deduplicating it first.
 */
export function modsFromConfigs(configs: readonly string[]): Mod[] {
  const mods = new Map<string, Mod>();

  for (const config of configs) {
    const mod = modFromConfig(config);
    if (mod) {
      mods.set(mod.root, mod);
    }
  }

  return [...mods.values()].sort(byName);
}

/** Undefined for anything that is not a `config.cpp` inside a named folder. */
function modFromConfig(config: string): Mod | undefined {
  const separator = config.lastIndexOf('/');

  // <= 0 covers both a bare file name and a config.cpp sitting at the root of a drive:
  // neither leaves a folder that could carry the mod's name.
  if (separator <= 0 || config.slice(separator + 1) !== CONFIG_FILE) {
    return undefined;
  }

  const root = config.slice(0, separator);
  const name = root.slice(root.lastIndexOf('/') + 1);
  if (name === '') {
    return undefined;
  }

  return { name, root, config };
}

/** Sibling mods of the same name (`CADCore\CADCore` next to a copy) keep a stable order by path. */
function byName(a: Mod, b: Mod): number {
  return a.name.localeCompare(b.name) || a.root.localeCompare(b.root);
}
