/**
 * Reading a `.enf`.
 *
 * A mod is configured by one file — `mod.enf` in its root, the way `package.json` configures a
 * package — and a monorepo may put a `workspace.enf` above them. The format is JSONC: text under
 * git, with comments, and with a JSON schema behind it so the editor completes the fields and
 * marks the mistakes as they are typed.
 *
 * Reading never throws and never gives up on a file: a manifest that is half-written still hands
 * back everything that was readable, and what was not comes back as a problem with the line and
 * column it sits on. A mod whose manifest is broken stays in the panel — with the mistake shown
 * next to it — rather than falling out of the list.
 *
 * The cascade rule is the one thing this file decides rather than reads: when a `workspace.enf`
 * exists it owns the launch block **whole**, and the block in `mod.enf` is ignored rather than
 * merged into it. Levels that merge are levels nobody can trace a setting through. See
 * `docs/adr/0002-enf-is-the-only-project-configuration.md`.
 *
 * The same shape is written out three times on purpose: here, for the extension, and once per file
 * in `schemas/`, for the editor. The schema is what the developer sees while typing; this is what
 * the extension trusts. `schemas.test.ts` is what keeps the two schemas from drifting apart.
 */

import { type Node, type ParseError, parseTree, printParseErrorCode } from 'jsonc-parser';
import { folderOf, isWithin, nameOf } from './paths';

/** The file a monorepo puts above its mods; a single mod does without one. */
export const WORKSPACE_FILE = 'workspace.enf';

/** What was read out of a manifest, and what could not be. */
export interface Parsed<T> {
  readonly value: T;
  readonly problems: readonly ManifestProblem[];
}

/** Something wrong with the file, at the place it is wrong. Lines and columns are 1-based. */
export interface ManifestProblem {
  readonly message: string;
  readonly line: number;
  readonly column: number;
}

/** `mod.enf`: what the mod says about itself, plus a launch block the workspace may override. */
export interface ModManifest {
  /**
   * The mod's one name: what the panel shows it as, what its prefix root goes onto the work drive
   * as (`P:\<name>`), and what it is built into (`@<name>`). The folder's own name if unset —
   * which is why a mod whose folder is called something else (`client`, holding
   * `CADNavigationClient`) is a mod that has to write its name down.
   */
  readonly name: string | undefined;
  readonly description: string | undefined;
  readonly author: string | undefined;
  readonly version: string | undefined;
  /** Globs the builder is not to pack: model sources, texture sources, whatever a pbo need not carry. */
  readonly exclude: readonly string[];
  readonly launch: Launch | undefined;
}

/** `workspace.enf`: nothing about any one mod, and the launch block for all of them. */
export interface WorkspaceManifest {
  readonly launch: Launch | undefined;
}

/** Everything about getting the game up: where the built mods are, who joins them, what to run. */
export interface Launch {
  /** Where `@<Mod>` folders live: the builder writes here and `-mod=` reads from here. */
  readonly modsDirectory: string | undefined;
  /**
   * Every mod the launch loads, by folder name and in load order. Both processes are given it as
   * `-mod=`. Mods of the workspace are not added to it on their own: one is loaded because it is
   * named here, exactly as a third-party one is.
   */
  readonly clientMods: readonly string[];
  /** The mods only the server loads, handed to it as `-serverMod=` on top of `clientMods`. */
  readonly serverMods: readonly string[];
  readonly targets: readonly Target[];
}

/** One entry of the Run and Debug list. */
export interface Target {
  readonly name: string;
  /** The mod the profile and the mission come from; the mod being launched if unset. */
  readonly mod: string | undefined;
  /** The world the server loads, which is the tail of the mission folder's name. */
  readonly map: string | undefined;
  readonly run: Run;
  /** The `server.cfg` to launch with, relative to the target's mod. */
  readonly serverConfig: string | undefined;
}

/** What a target puts up. */
export type Run = 'client' | 'server' | 'both';

/** A mod's configuration with the cascade rule applied: what it says, and the one launch block. */
export interface Configuration {
  readonly manifest: ModManifest;
  readonly launch: Launch;
}

/** A file as the search found it: where it is and what it holds. */
export interface ManifestSource {
  readonly path: string;
  readonly source: string;
}

/** All the cascade needs to know about a mod: where it sits, and which file configures it. */
export interface ModLocation {
  readonly root: string;
  readonly manifest: string | undefined;
}

/** One mod's configuration, and what the file it was read from got wrong. */
export interface Configured {
  readonly configuration: Configuration;
  readonly problems: readonly ManifestProblem[];
  /** The `workspace.enf` whose launch block this mod obeys, where one does. */
  readonly workspace: string | undefined;
}

/** Every mod's configuration, and every workspace file, both keyed by the path they came from. */
export interface Configurations {
  readonly mods: ReadonlyMap<string, Configured>;
  readonly workspaces: ReadonlyMap<string, readonly ManifestProblem[]>;
}

export function readMod(source: string): Parsed<ModManifest> {
  const reading = read(source);
  const root = reading.root;

  const value: ModManifest = {
    name: reading.text(root, 'name'),
    description: reading.text(root, 'description'),
    author: reading.text(root, 'author'),
    version: reading.text(root, 'version'),
    exclude: reading.texts(root, 'exclude'),
    launch: launchOf(reading, root),
  };
  reading.only(root, MOD_FIELDS);

  return { value, problems: reading.problems };
}

export function readWorkspace(source: string): Parsed<WorkspaceManifest> {
  const reading = read(source);
  const root = reading.root;

  const value: WorkspaceManifest = { launch: launchOf(reading, root) };
  reading.only(root, WORKSPACE_FIELDS);

  return { value, problems: reading.problems };
}

/**
 * The cascade: a `workspace.enf` owns the launch block by existing, so a mod under one is launched
 * the way the workspace says even when the workspace says nothing at all. Without one the mod
 * owns its own.
 */
export function configurationOf(
  manifest: ModManifest,
  workspace: WorkspaceManifest | undefined,
): Configuration {
  const launch = (workspace ? workspace.launch : manifest.launch) ?? NO_LAUNCH;

  return { manifest, launch };
}

/**
 * The same, for a whole workspace at once: every mod read, matched with the `workspace.enf`
 * nearest above it, and left with one launch block. This is the join in full — which file
 * configures which mod is decided here rather than by whoever did the reading.
 */
export function configurationsOf(
  mods: readonly ModLocation[],
  files: readonly ManifestSource[],
): Configurations {
  const workspaces = new Map(
    files
      .filter((file) => nameOf(file.path) === WORKSPACE_FILE)
      .map((file) => [file.path, readWorkspace(file.source)] as const),
  );
  const sources = new Map(files.map((file) => [file.path, file.source] as const));
  const above = [...workspaces.keys()];

  const configured = mods.flatMap((mod) => {
    if (mod.manifest === undefined) {
      return [];
    }

    const read = readMod(sources.get(mod.manifest) ?? '');
    const workspace = workspaceFor(mod.root, above);
    const owner = workspace === undefined ? undefined : workspaces.get(workspace)?.value;

    return [
      [
        mod.manifest,
        { configuration: configurationOf(read.value, owner), problems: read.problems, workspace },
      ] as const,
    ];
  });

  return {
    mods: new Map(configured),
    workspaces: new Map([...workspaces].map(([path, read]) => [path, read.problems])),
  };
}

/**
 * The `workspace.enf` a mod answers to: the nearest one above it. A workspace holding two
 * monorepos side by side, or a monorepo opened from a folder above it, still leaves every mod
 * with one file to obey.
 */
export function workspaceFor(modRoot: string, files: readonly string[]): string | undefined {
  return files
    .filter((file) => isWithin(modRoot, folderOf(file)))
    .sort((a, b) => folderOf(b).length - folderOf(a).length)
    .at(0);
}

/** What a mod that configures no launch is launched by, which is nothing at all. */
export const NO_LAUNCH: Launch = {
  modsDirectory: undefined,
  clientMods: [],
  serverMods: [],
  targets: [],
};

/** `$schema` is how a developer points an editor at the schema, so every file may carry it. */
const SCHEMA_FIELD = '$schema';

/*
 * The fields of each block, in the order the schema writes them. Two jobs at once: a key on none
 * of these lists is a field nobody declared, and the order is where the form puts a field it is
 * adding — so a manifest filled in by the form reads the way one written by hand does.
 */

export const MOD_FIELDS = [
  SCHEMA_FIELD,
  'name',
  'description',
  'author',
  'version',
  'exclude',
  'launch',
];

export const WORKSPACE_FIELDS = [SCHEMA_FIELD, 'launch'];

export const LAUNCH_FIELDS = ['modsDirectory', 'clientMods', 'serverMods', 'targets'];

export const TARGET_FIELDS = ['name', 'mod', 'map', 'run', 'serverConfig'];

const RUN: readonly Run[] = ['client', 'server', 'both'];

function launchOf(reading: Reading, root: Node | undefined): Launch | undefined {
  const node = reading.block(root, 'launch');
  if (node === undefined) {
    return undefined;
  }

  const launch: Launch = {
    modsDirectory: reading.text(node, 'modsDirectory'),
    clientMods: reading.texts(node, 'clientMods'),
    serverMods: reading.texts(node, 'serverMods'),
    targets: reading.items(node, 'targets').flatMap((item) => targetOf(reading, item)),
  };
  reading.only(node, LAUNCH_FIELDS);

  return launch;
}

/** A target nobody can pick — one with no name — is dropped rather than shown blank in the list. */
function targetOf(reading: Reading, node: Node): Target[] {
  if (node.type !== 'object') {
    reading.report(node, 'A target must be an object.');
    return [];
  }

  const name = reading.text(node, 'name');
  if (name === undefined) {
    reading.report(node, 'A target must have a "name": it is what the Run and Debug list shows.');
    return [];
  }

  const target: Target = {
    name,
    mod: reading.text(node, 'mod'),
    map: reading.text(node, 'map'),
    run: reading.choice(node, 'run', RUN, 'both'),
    serverConfig: reading.text(node, 'serverConfig'),
  };
  reading.only(node, TARGET_FIELDS);

  return [target];
}

/**
 * One pass over one file: the tree it parsed to, the problems found so far, and the readers that
 * add to them. Every reader answers with something usable — a missing field and a field of the
 * wrong type read the same way — so a broken manifest still yields a configuration.
 */
interface Reading {
  /** The manifest object, or undefined when the file holds anything else. */
  readonly root: Node | undefined;
  readonly problems: readonly ManifestProblem[];
  report(node: Node, message: string): void;
  text(object: Node | undefined, field: string): string | undefined;
  texts(object: Node | undefined, field: string): string[];
  choice<T extends string>(
    object: Node | undefined,
    field: string,
    allowed: readonly T[],
    fallback: T,
  ): T;
  block(object: Node | undefined, field: string): Node | undefined;
  items(object: Node | undefined, field: string): Node[];
  only(object: Node | undefined, fields: readonly string[]): void;
}

function read(source: string): Reading {
  const problems: ManifestProblem[] = [];
  const lines = lineStarts(source);
  const errors: ParseError[] = [];

  // An empty file is a mod with nothing configured, which is what a mod starts as.
  const tree = parseTree(source, errors, { allowTrailingComma: true, allowEmptyContent: true });

  const report = (node: Node, message: string): void => {
    problems.push({ message, ...positionOf(lines, node.offset) });
  };

  for (const error of errors) {
    problems.push({ message: describe(error), ...positionOf(lines, error.offset) });
  }

  const root = rootOf(tree, report);

  const text = (object: Node | undefined, field: string): string | undefined => {
    const node = memberOf(object, field);
    if (node === undefined) {
      return undefined;
    }

    if (node.type !== 'string') {
      report(node, `"${field}" must be a string.`);
      return undefined;
    }

    return textOf(node);
  };

  const array = (object: Node | undefined, field: string, expected: string): Node[] | undefined => {
    const node = memberOf(object, field);
    if (node === undefined) {
      return undefined;
    }

    if (node.type !== 'array') {
      report(node, `"${field}" must be ${expected}.`);
      return undefined;
    }

    return node.children ?? [];
  };

  return {
    root,
    problems,
    report,
    text,

    texts: (object, field) =>
      (array(object, field, 'an array of strings') ?? []).flatMap((item) => {
        if (item.type !== 'string') {
          report(item, `Every item of "${field}" must be a string.`);
          return [];
        }

        return [textOf(item)];
      }),

    choice: (object, field, allowed, fallback) => {
      const value = text(object, field);
      if (value === undefined) {
        return fallback;
      }

      const found = allowed.find((option) => option === value);
      if (found === undefined) {
        const node = memberOf(object, field);
        if (node) {
          report(node, `"${field}" must be one of: ${allowed.join(', ')}.`);
        }
        return fallback;
      }

      return found;
    },

    block: (object, field) => {
      const node = memberOf(object, field);
      if (node === undefined) {
        return undefined;
      }

      if (node.type !== 'object') {
        report(node, `"${field}" must be an object.`);
        return undefined;
      }

      return node;
    },

    items: (object, field) => array(object, field, 'an array') ?? [],

    only: (object, fields) => {
      for (const key of keysOf(object)) {
        if (!fields.includes(textOf(key))) {
          report(key, `Unknown field "${textOf(key)}".`);
        }
      }
    },
  };
}

/** Anything but an object — a list, a string, a number — configures nothing. */
function rootOf(
  tree: Node | undefined,
  report: (node: Node, message: string) => void,
): Node | undefined {
  if (tree === undefined) {
    return undefined;
  }

  if (tree.type !== 'object') {
    report(tree, 'A manifest must be an object.');
    return undefined;
  }

  return tree;
}

/** The value of the field, taking the last of a repeated key the way `JSON.parse` does. */
function memberOf(object: Node | undefined, field: string): Node | undefined {
  if (object?.type !== 'object') {
    return undefined;
  }

  const property = (object.children ?? [])
    .filter((child) => child.children?.[0] !== undefined && textOf(child.children[0]) === field)
    .at(-1);

  return property?.children?.[1];
}

function keysOf(object: Node | undefined): Node[] {
  if (object?.type !== 'object') {
    return [];
  }

  return (object.children ?? []).flatMap((property) => {
    const key = property.children?.[0];
    return key ? [key] : [];
  });
}

/** `Node.value` is `any`, and this is the one place that ends. */
function textOf(node: Node): string {
  return typeof node.value === 'string' ? node.value : '';
}

/** The parser names its errors in code; a developer reads them as a sentence. */
function describe(error: ParseError): string {
  const words = printParseErrorCode(error.error).replace(/([a-z])([A-Z])/g, '$1 $2');
  return `${words.slice(0, 1)}${words.slice(1).toLowerCase()}.`;
}

function lineStarts(source: string): number[] {
  const starts = [0];

  for (let at = source.indexOf('\n'); at !== -1; at = source.indexOf('\n', at + 1)) {
    starts.push(at + 1);
  }

  return starts;
}

function positionOf(
  starts: readonly number[],
  offset: number,
): { line: number; column: number } {
  let line = 0;
  while (line + 1 < starts.length && (starts[line + 1] ?? 0) <= offset) {
    line += 1;
  }

  return { line: line + 1, column: offset - (starts[line] ?? 0) + 1 };
}
