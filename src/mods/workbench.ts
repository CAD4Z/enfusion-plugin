/**
 * The Workbench project of a mod: which one Workbench opens, what a new one is written with, and
 * how a mod made later is written into one that is already there.
 *
 * A project is a file discovered under the mod root. `dayz.gproj` is the DayZ convention and wins
 * where a mod keeps helper projects beside it; otherwise the shallowest, alphabetically first
 * project is deterministic and visible in the button tooltip before anything is started. A mod of
 * a workspace that has none of its own opens the workspace's: one project for every mod of it,
 * which is what lets one Workbench compile them all together.
 *
 * A project written here is DayZ Tools' own `dayz.gproj` with two things changed. Its file system
 * is the work drive rather than the folder Workbench was started in, and every script module lists
 * the mods' folders after the vanilla ones — which is what the script editor compiles, so a mod
 * missing from it is a mod Workbench has never heard of.
 */

import type { ModDefs, ScriptModuleName } from './config';
import { folderOf, nameOf, samePath } from './paths';

/** The folder a project is written into, beside the prefix root rather than inside it. */
export const PROJECT_FOLDER = 'Workbench';

/** What Workbench reads out of the folder it was started in, so the one name a project takes. */
export const PROJECT_FILE = 'dayz.gproj';

/**
 * The `.gproj` Workbench should open for this mod root: its own, or else the one of the workspace
 * whose folder is given — the workspace the mod answers to.
 */
export function projectOf(
  modRoot: string,
  projects: readonly string[],
  workspaceRoot?: string,
): string | undefined {
  return (
    projectUnder(modRoot, projects) ??
    (workspaceRoot === undefined ? undefined : workspaceProjectOf(workspaceRoot, projects))
  );
}

/**
 * The workspace's own project, which is the one in its `Workbench` folder and nowhere else. A
 * workspace holds all sorts besides its mods — a copy of a project to try something on, a project
 * of some tool — and any `.gproj` anywhere in it would be a project nobody chose.
 */
export function workspaceProjectOf(workspaceRoot: string, projects: readonly string[]): string | undefined {
  const folder = samePath(`${workspaceRoot}/${PROJECT_FOLDER}`);

  return projectUnder(
    workspaceRoot,
    projects.filter((project) => samePath(folderOf(project)) === folder),
  );
}

function projectUnder(folder: string, projects: readonly string[]): string | undefined {
  const root = samePath(folder);

  return projects
    .filter((project) => {
      const path = samePath(project);
      return path.startsWith(`${root}/`) && path.endsWith('.gproj');
    })
    .sort((a, b) => compareProject(a, b))[0];
}

function compareProject(a: string, b: string): number {
  const conventional = Number(!isDayzProject(a)) - Number(!isDayzProject(b));
  const depth = samePath(a).split('/').length - samePath(b).split('/').length;

  return conventional || depth || a.localeCompare(b);
}

function isDayzProject(path: string): boolean {
  return nameOf(samePath(path)) === PROJECT_FILE;
}

/**
 * What Workbench is started with: its logs on, and the mod root as the repository the Workbench
 * plugins read — the two arguments `LaunchWorkbench.bat` passed. One command line rather than a
 * list, because the shell that starts Workbench takes one.
 *
 * The repository is quoted the way a Windows program splits its command line. A path cannot hold
 * a quotation mark, so quoting is all it takes — save for backslashes in front of the closing
 * quote: a mod root at `F:\` would end in `\"`, which is a quotation mark kept rather than a
 * quote closed, so those are doubled.
 */
export function workbenchArgumentsOf(repository: string): string {
  const quoted = `-repository=${repository}`.replace(/\\+$/, (slashes) => slashes + slashes);

  return `-doLogs "${quoted}"`;
}

/**
 * A whole project for these mods, in the order they are given — which is build order, so that a
 * mod's scripts come after those of the mods it is built on, the way the game compiles them.
 *
 * `drive` is the work drive's letter, which is the machine's and not the mod's: it is written in
 * once, when the project is, and a machine with another letter changes that one line.
 */
export function projectFileOf(drive: string, title: string, mods: readonly ModDefs[]): string {
  return [
    'GameProjectClass {',
    line(1, 'ID "DayZ"'),
    line(1, `TITLE ${quoted(title)}`),
    line(1, 'Configurations {'),
    line(2, 'GameProjectConfigClass PC {'),
    line(3, 'platformHardware PC'),
    line(3, 'skeletonDefinitions "DZ/Anims/cfg/skeletons.anim.xml"'),
    line(3, 'FileSystem {'),
    line(4, 'FileSystemPathClass {'),
    line(5, 'Name "Workdrive"'),
    line(5, `Directory ${quoted(`${drive}/`)}`),
    line(4, '}'),
    line(3, '}'),
    ...listOf(3, IMAGE_SETS, merged(VANILLA_IMAGE_SETS, mods.flatMap((mod) => mod.imageSets))),
    ...listOf(3, WIDGET_STYLES, merged(VANILLA_WIDGET_STYLES, mods.flatMap((mod) => mod.widgetStyles))),
    line(3, 'PhysicsSettings PhysicsSettingsClass "{50C8B06FB4D5FA52}" {'),
    line(4, 'GridBP 0'),
    line(4, 'Optimize 16'),
    line(3, '}'),
    line(3, 'ScriptModules {'),
    ...MODULES.flatMap((module) => [
      line(4, 'ScriptModulePathClass {'),
      line(5, `Name ${quoted(module.name)}`),
      ...listOf(5, 'Paths', merged(module.vanilla, scriptsOf(mods, module.name))),
      line(5, `EntryPoint ${quoted(module.entryPoint)}`),
      line(4, '}'),
    ]),
    line(3, '}'),
    line(2, '}'),
    ...['XBOX_ONE', 'PS4', 'LINUX'].flatMap((platform) => [
      line(2, `GameProjectConfigClass ${platform} {`),
      line(3, `platformHardware ${platform}`),
      line(2, '}'),
    ]),
    line(1, '}'),
    '}',
    '',
  ].join('\n');
}

/**
 * A project with one more mod's folders written into it, or undefined where the project has no
 * place for one of them: a module it does not list, or no PC configuration at all. A folder the
 * project lists already is left where it is, and so is everything else in the file — comments,
 * the order of the lists, the modules nobody asked about.
 */
export function withProjectMod(source: string, mod: ModDefs): string | undefined {
  const project = blocksOf(source).find((block) => block.header === 'GameProjectClass');
  const pc = child(child(project, 'Configurations'), /^GameProjectConfigClass\s+PC$/);
  if (pc === undefined) {
    return undefined;
  }

  const modules = child(pc, 'ScriptModules')?.children.filter((block) => block.header === 'ScriptModulePathClass') ?? [];
  const insertions: Insertion[] = [];

  for (const module of MODULES) {
    const wanted = module.name === WORKBENCH ? [] : mod.scripts[module.name];
    if (wanted.length === 0) {
      continue;
    }

    const paths = child(
      modules.find((block) => NAME.exec(ownTextOf(source, block))?.[1] === module.name),
      'Paths',
    );
    if (paths === undefined) {
      return undefined;
    }
    insertions.push(...insertionOf(source, paths, wanted));
  }

  // A mod's image sets and widget styles are for the layout editor's pickers, and a project that
  // lists none of its own has not got the list to add them to: that is no reason to refuse it.
  for (const [header, wanted] of [
    [IMAGE_SETS, mod.imageSets],
    [WIDGET_STYLES, mod.widgetStyles],
  ] as const) {
    const list = child(pc, header);
    if (list !== undefined && wanted.length > 0) {
      insertions.push(...insertionOf(source, list, wanted));
    }
  }

  // From the end backwards, so that every offset still means what it meant when it was found.
  return insertions
    .sort((a, b) => b.at - a.at)
    .reduce((text, insertion) => text.slice(0, insertion.at) + insertion.text + text.slice(insertion.at), source);
}

/** The modules DayZ's own project lists, with what it puts in each. */
const MODULES: readonly {
  readonly name: ScriptModuleName | typeof WORKBENCH;
  readonly vanilla: readonly string[];
  readonly entryPoint: string;
}[] = [
  { name: 'core', vanilla: ['scripts/1_Core'], entryPoint: '' },
  { name: 'gameLib', vanilla: ['scripts/2_GameLib'], entryPoint: '' },
  { name: 'game', vanilla: ['scripts/3_Game'], entryPoint: 'CreateGame' },
  { name: 'world', vanilla: ['scripts/4_World'], entryPoint: '' },
  { name: 'mission', vanilla: ['scripts/5_Mission'], entryPoint: 'CreateMission' },
  { name: 'workbench', vanilla: ['scripts/editor/Workbench', 'scripts/editor/plugins'], entryPoint: '' },
];

/** The module Workbench's own plugins are compiled into, which no mod's `CfgMods` attaches to. */
const WORKBENCH = 'workbench';

const IMAGE_SETS = 'imageSets';

const WIDGET_STYLES = 'widgetStyles';

/** What DayZ Tools' own project lists, so a layout edited in Workbench draws as the game draws it. */
const VANILLA_IMAGE_SETS = [
  'gui/imagesets/BleedingDrops.imageset',
  'gui/imagesets/ccgui_enforce.imageset',
  'gui/imagesets/rover_imageset.imageset',
  'gui/imagesets/dayz_gui.imageset',
  'gui/imagesets/dayz_crosshairs.imageset',
  'gui/imagesets/dayz_inventory.imageset',
  'gui/imagesets/xbox_buttons.imageset',
  'gui/imagesets/playstation_buttons.imageset',
  'gui/imagesets/console_toolbar.imageset',
  'gui/imagesets/Map2D_UI.imageset',
  'Graphics/Textures/postprocess/VignetteFrames.imageset',
  'gui/imagesets/dayz_additional_gui.imageset',
];

const VANILLA_WIDGET_STYLES = ['gui/looknfeel/dayzwidgets.styles'];

/** `Name "game"`, which is how a module of the project says which one it is. */
const NAME = /\bName\s+"([^"]*)"/;

function scriptsOf(mods: readonly ModDefs[], module: ScriptModuleName | typeof WORKBENCH): string[] {
  return module === WORKBENCH ? [] : mods.flatMap((mod) => mod.scripts[module]);
}

/** The first list, then what of the second it lacks, compared the way Windows compares paths. */
function merged(first: readonly string[], second: readonly string[]): string[] {
  const seen = new Set<string>();

  return [...first, ...second].filter((path) => {
    const key = samePath(path);
    const fresh = !seen.has(key);
    seen.add(key);
    return fresh;
  });
}

function listOf(depth: number, header: string, items: readonly string[]): string[] {
  return [line(depth, `${header} {`), ...items.map((item) => line(depth + 1, quoted(item))), line(depth, '}')];
}

function line(depth: number, text: string): string {
  return `${'\t'.repeat(depth)}${text}`;
}

/** A value the way a project writes one; no path or name it is given can hold a quotation mark. */
function quoted(value: string): string {
  return `"${value}"`;
}

/** One `{ … }` of a project: what is written in front of it, where it is, and what it holds. */
interface Block {
  /** What the line says before the brace: `Paths`, `GameProjectConfigClass PC`. */
  readonly header: string;
  /** The opening brace, and the closing one. */
  readonly open: number;
  close: number;
  readonly children: Block[];
}

/** Text to put in at an offset of the source. */
interface Insertion {
  readonly at: number;
  readonly text: string;
}

/**
 * Every block of the project, nested the way they are written. The format is a line per member,
 * so a block's header is the last line written before its brace — on the brace's own line, or on
 * the line above where the brace was put on a line of its own. Braces inside a quoted string — a
 * resource GUID is written `"{50C8B06FB4D5FA52}"` — and inside a comment are not blocks at all.
 */
function blocksOf(source: string): Block[] {
  const root: Block = { header: '', open: -1, close: source.length, children: [] };
  const open: Block[] = [root];
  // Where the text that can be a header starts: just past the last brace.
  let from = 0;

  for (let at = 0; at < source.length; at += 1) {
    const char = source[at];

    if (char === '"') {
      const end = source.indexOf('"', at + 1);
      at = end === -1 ? source.length : end;
    } else if (char === '/' && source[at + 1] === '/') {
      const end = source.indexOf('\n', at);
      at = end === -1 ? source.length : end;
    } else if (char === '/' && source[at + 1] === '*') {
      const end = source.indexOf('*/', at + 2);
      at = end === -1 ? source.length : end + 1;
    } else if (char === '{') {
      const header =
        source
          .slice(from, at)
          .split('\n')
          .map((line) => line.replace(/\/\/.*$/, '').trim())
          .filter((line) => line !== '')
          .at(-1) ?? '';
      const block: Block = { header, open: at, close: source.length, children: [] };
      open[open.length - 1].children.push(block);
      open.push(block);
      from = at + 1;
    } else if (char === '}') {
      if (open.length > 1) {
        const block = open.pop();
        if (block !== undefined) {
          block.close = at;
        }
      }
      from = at + 1;
    }
  }

  return root.children;
}

/** The first block inside this one whose header is this, or matches this. */
function child(block: Block | undefined, header: string | RegExp): Block | undefined {
  return block?.children.find((inner) =>
    typeof header === 'string' ? inner.header === header : header.test(inner.header),
  );
}

/** What the block says itself, with the blocks inside it left out. */
function ownTextOf(source: string, block: Block): string {
  let text = '';
  let at = block.open + 1;

  for (const inner of block.children) {
    text += source.slice(at, inner.open);
    at = inner.close + 1;
  }

  return text + source.slice(at, block.close);
}

/**
 * The items of a list it lacks, written the way the list is written: a line each under the last
 * item where the list is a line per item, and along the line where it is written on one.
 */
function insertionOf(source: string, list: Block, wanted: readonly string[]): Insertion[] {
  const items = [...ownTextOf(source, list).matchAll(/"([^"]*)"/g)].map((match) => samePath(match[1]));
  const missing = wanted.filter((item, at) => {
    const key = samePath(item);
    return !items.includes(key) && wanted.findIndex((other) => samePath(other) === key) === at;
  });
  if (missing.length === 0) {
    return [];
  }

  const inside = source.slice(list.open + 1, list.close);
  if (!inside.includes('\n')) {
    const written = inside.replace(/\s+$/, '');
    const added = missing.map((item) => ` ${quoted(item)}`).join('');

    return [{ at: list.open + 1 + written.length, text: added }];
  }

  const newline = source.includes('\r\n') ? '\r\n' : '\n';
  const closingLine = source.lastIndexOf('\n', list.close - 1) + 1;
  const last = inside.replace(/\s+$/, '');
  const indent =
    last.trim() === ''
      ? `${indentAt(source, list.open)}\t`
      : indentAt(source, list.open + 1 + last.length);

  // The closing brace on a line of its own is the usual case, and the new lines go in above it;
  // one written after the last item is moved down rather than having the items go in behind it.
  if (source.slice(closingLine, list.close).trim() === '') {
    return [{ at: closingLine, text: missing.map((item) => `${indent}${quoted(item)}${newline}`).join('') }];
  }

  return [{ at: list.open + 1 + last.length, text: missing.map((item) => `${newline}${indent}${quoted(item)}`).join('') }];
}

/** What the line holding this offset is indented by. */
function indentAt(source: string, at: number): string {
  const start = source.lastIndexOf('\n', at - 1) + 1;

  return /^[ \t]*/.exec(source.slice(start, at))?.[0] ?? '';
}
