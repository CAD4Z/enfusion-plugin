/**
 * Editing a `.enf` by a form.
 *
 * The file stays what it was: text under git, in JSONC, with the comments and the field order
 * whoever wrote it chose. So the form never renders the manifest back out — it makes one surgical
 * change per move, replacing the span the changed field occupies and nothing else. What the
 * developer sees in the diff is the line they changed.
 *
 * That surgery needs a tree to aim at, which a file with a syntax error has not got. Such a file
 * is not a blank form: it is read as far as it was readable — `readMod` hands back everything it
 * managed — and shown with what is wrong with it, and the form writes nothing until the text
 * editor has been through it. Losing the half of a manifest that parsed because the other half is
 * missing a comma is the one thing a form over a text file must never do.
 *
 * The other way the form can be wrong about a file is subtler and refused for the same reason: a
 * form points at a field by its name and at a row by where it sits, so a file where those two do
 * not mean what they say — a key written twice, a list the reader could not read whole — is a file
 * where the next edit would land somewhere the developer is not looking.
 *
 * Nothing here goes near a document, an editor or a disk: a move in comes out as the spans of the
 * file to replace, which is what makes the whole of it comparable in a test.
 *
 * See `docs/adr/0003-form-edits-the-text.md`.
 */

import {
  type FormattingOptions,
  type ModificationOptions,
  type Node,
  type ParseError,
  findNodeAtLocation,
  modify,
  parseTree,
} from 'jsonc-parser';
import {
  LAUNCH_FIELDS,
  type Launch,
  MOD_FIELDS,
  type ManifestProblem,
  type ModManifest,
  NO_LAUNCH,
  TARGET_FIELDS,
  WORKSPACE_FIELDS,
  readMod,
  readWorkspace,
} from './enf';

/** Which of the two files the form is over, which is what fields it has. */
export type ManifestKind = 'mod' | 'workspace';

/** One file as the form shows it: what was read, what was not, and what may be written back. */
export interface Form {
  readonly kind: ManifestKind;
  /** What the mod says about itself; a `workspace.enf` says nothing about any one mod. */
  readonly mod: ModManifest | undefined;
  /**
   * The launch block, lifted out of whichever file this is so that one set of fields shows it in
   * both. Never undefined: a file with no block is a form with empty fields, not a form without.
   */
  readonly launch: Launch;
  readonly problems: readonly ManifestProblem[];
  /** Why the form will not write into this file; undefined when it will. */
  readonly refusal: string | undefined;
}

/** One move of the form: what it does, and where in the file it does it. */
export type FormEdit =
  /** Writes the field; an empty value clears it, an empty box being a field left unanswered. */
  | { readonly kind: 'set'; readonly path: FormPath; readonly value: string }
  /** Takes out whatever is at this path: a field, an item of a list, a whole target. */
  | { readonly kind: 'clear'; readonly path: FormPath }
  /** Adds to the end of the list at this path. */
  | { readonly kind: 'append'; readonly path: FormPath; readonly value: string }
  /** Adds a target to the launch block, named so that no other target of the file is. */
  | { readonly kind: 'addTarget' };

/** Where in the file: field names and list indices, the way the parser counts them. */
export type FormPath = readonly (string | number)[];

/** A span of the file to replace, which is what the editor turns into one edit of its own. */
export interface TextChange {
  readonly offset: number;
  readonly length: number;
  readonly content: string;
}

/**
 * The file read as far as it goes, which for a broken one is further than nothing. The two files
 * are read by their own readers — they declare different fields, and a key on neither list is a
 * field nobody declared — and come out as the one shape the form is built from.
 */
export function formOf(kind: ManifestKind, source: string): Form {
  const read = readOf(kind, source);

  return {
    kind,
    mod: read.mod,
    launch: read.launch,
    problems: read.problems,
    refusal: refusalOf(kind, source),
  };
}

/**
 * Why the form will not write into this file, or undefined when it will.
 *
 * A form edit is surgery: it finds the field in the parsed tree and replaces that span, which is
 * what leaves every comment and every other field exactly as they were. Two things can leave it
 * with nothing to aim at, and both end the same way — the file is shown as far as it was read, and
 * the text editor, where the mistake is underlined already, is where it gets fixed.
 *
 * The file did not parse whole, so there is no tree. Or it parsed into something the form would
 * misaddress: a key written twice, where what is shown is the second answer and what an edit lands
 * on is the first; a list the reader dropped an item out of, where every row after the gap sits at
 * a place in the file one lower than it looks.
 */
export function refusalOf(kind: ManifestKind, source: string): string | undefined {
  const errors: ParseError[] = [];
  const root = parseTree(source, errors, PARSING);

  if (errors.length > 0) {
    return (
      'This file has a syntax error, so the form is showing what was readable and will not write ' +
      'over the rest. Fix it as text — the mistakes are listed here and underlined there — and ' +
      'the form takes over again.'
    );
  }

  if (root !== undefined && root.type !== 'object') {
    return (
      `A manifest is an object, and this file holds ${root.type === 'array' ? 'a list' : 'a value'}. ` +
      'Make it one as text and the form takes over again.'
    );
  }

  const twice = repeatedIn(root);
  if (twice !== undefined) {
    return (
      `"${twice}" is written twice in this file. The form would show one of them and write into ` +
      'the other, so it writes into neither: take the spare one out as text.'
    );
  }

  const dropped = droppedFrom(kind, root, source);
  if (dropped !== undefined) {
    return (
      `Something in "${dropped}" could not be read, so the rows the form shows are no longer where ` +
      'the file writes them, and an edit would land on the wrong one. It is listed here and ' +
      'underlined in the text editor; fix it there and the form takes over again.'
    );
  }

  return undefined;
}

/**
 * What one move of the form comes to, as the spans of the file to replace. Empty where nothing is
 * to be written: a file the form refuses, a move that would add nothing, a path the file is not
 * shaped for, and a move that would put back exactly what is there.
 */
export function changesOf(
  kind: ManifestKind,
  source: string,
  edit: FormEdit,
): readonly TextChange[] {
  if (refusalOf(kind, source) !== undefined) {
    return [];
  }

  const write = writeOf(kind, source, edit);
  if (write === undefined) {
    return [];
  }

  // Taking something out is done here rather than by `modify`, which takes the neighbours'
  // comments with it; everything else the parser's own editing does exactly right.
  const changes =
    write.value === undefined ? removalOf(source, write.path) : written(kind, source, write);

  return real(source, changes);
}

/**
 * A name no target of this file has yet. A target is picked out of the Run and Debug list by its
 * name, so two of them called the same thing is the one state a new one must not arrive in.
 */
function targetNameOf(taken: readonly string[]): string {
  const used = new Set(taken);
  // One more candidate than there are targets, so one of them is always free.
  const candidates = [
    NEW_TARGET,
    ...Array.from({ length: taken.length + 1 }, (_, at) => `${NEW_TARGET} ${at + 2}`),
  ];

  return candidates.find((candidate) => !used.has(candidate)) ?? NEW_TARGET;
}

/** What a target with nothing said about it yet is called. */
const NEW_TARGET = 'Target';

/** The two fields the form reaches through, which the reader knows by the same words. */
const LAUNCH = 'launch';

const TARGETS = 'targets';

/** The last segment that means "at the end of this list" to the parser's editing side. */
const APPEND = -1;

/**
 * What a file with nothing to go on is indented by, which is what this extension writes its own
 * manifests with. Tabs land here too and ignore it: an indent of them is one tab per level.
 */
const INDENT_WIDTH = 2;

/** The first line that is indented at all, which is what the rest of the file is indented like. */
const INDENT = /^([ \t]+)\S/m;

/** A line holding a comment and nothing else, in either of the two ways one is written. */
const COMMENT_LINE = /^\s*(\/\/|\/\*.*\*\/\s*$)/;

/** How a `.enf` is read: JSONC, and an empty file is a manifest that says nothing. */
const PARSING = { allowTrailingComma: true, allowEmptyContent: true };

/** The lists the form shows a row per item of, which is what makes their length matter. */
const MOD_LISTS: readonly FormPath[] = [
  ['exclude'],
  [LAUNCH, 'clientMods'],
  [LAUNCH, 'serverMods'],
  [LAUNCH, TARGETS],
];

const WORKSPACE_LISTS: readonly FormPath[] = MOD_LISTS.filter((path) => path[0] === LAUNCH);

/** The manifest as the form's own two questions want it: the fields, and what went wrong. */
interface Read {
  readonly mod: ModManifest | undefined;
  readonly launch: Launch;
  readonly problems: readonly ManifestProblem[];
}

function readOf(kind: ManifestKind, source: string): Read {
  if (kind === 'workspace') {
    const read = readWorkspace(source);
    return { mod: undefined, launch: read.value.launch ?? NO_LAUNCH, problems: read.problems };
  }

  const read = readMod(source);
  return { mod: read.value, launch: read.value.launch ?? NO_LAUNCH, problems: read.problems };
}

/** Where the write lands and what goes there; `undefined` takes out whatever is there now. */
interface Write {
  readonly path: FormPath;
  readonly value: string | Record<string, string> | undefined;
  readonly appending: boolean;
}

function writeOf(kind: ManifestKind, source: string, edit: FormEdit): Write | undefined {
  switch (edit.kind) {
    case 'set':
      return {
        path: edit.path,
        value: edit.value === '' ? undefined : edit.value,
        appending: false,
      };

    case 'clear':
      return { path: edit.path, value: undefined, appending: false };

    // An empty box added nothing, so nothing is written: the row was a place to type, not a value.
    case 'append':
      return edit.value === ''
        ? undefined
        : { path: [...edit.path, APPEND], value: edit.value, appending: true };

    case 'addTarget':
      return {
        path: [LAUNCH, TARGETS, APPEND],
        value: { name: targetNameOf(readOf(kind, source).launch.targets.map((it) => it.name)) },
        appending: true,
      };
  }
}

/** Everything but taking something out, which the parser's own editing side does right. */
function written(kind: ManifestKind, source: string, write: Write): readonly TextChange[] {
  // Formatting is what puts an added field on a line of its own. A value written over one already
  // there needs none of it: the span is swapped, and how the file was laid out around it — a list
  // written on one line, a comment sitting beside it — is left exactly as it was found. Which is
  // the case a developer typing in a box is in.
  const options: ModificationOptions = replacing(source, write)
    ? {}
    : {
        formattingOptions: formattingOf(source),
        getInsertionIndex: insertionIndexOf(kind, write.path),
        isArrayInsertion: write.appending,
      };

  try {
    return modify(source, [...write.path], write.value, options).map(spanOf);
  } catch {
    // `modify` throws where the file is not shaped the way the path expects — a `launch` written
    // as a string, a `targets` written as an object. That is a problem the form is already showing
    // and the text editor is the place to fix; nothing is written over it here.
    return [];
  }
}

/**
 * Taking a field or an item out, leaving every comment that was not its own.
 *
 * This is the one edit the parser's own editing side is not asked to do, for two reasons. It takes
 * everything between the thing and its neighbour, which is more than the thing: the note at the
 * end of the line above belongs to the field above, and the note on the line below to the field
 * below, and both go with it. And its removal of the last item of a list counts the closing
 * bracket wrong wherever no line break sits in front of one, which leaves the file unparseable.
 *
 * What goes instead is the thing and what is the thing's: the lines it is written on, the comment
 * and blank lines above it that no neighbour is written on, and the comma that joined it. An item
 * sharing its line with its neighbours has no lines of its own, so there it is the span that goes
 * — and a line like that carries no comments to lose.
 */
function removalOf(source: string, path: FormPath): readonly TextChange[] {
  const root = parseTree(source, [], PARSING);
  const found = root === undefined ? undefined : findNodeAtLocation(root, [...path]);
  if (found === undefined) {
    return [];
  }

  // In an object it is the property that goes, key and all; in a list it is the item itself.
  const item = found.parent?.type === 'property' ? found.parent : found;
  const parent = item.parent;
  const children = parent?.children ?? [];
  const at = children.indexOf(item);
  if (parent === undefined || at === -1) {
    return [];
  }

  // The last of them, which takes the whole inside of the brackets and no comma with it.
  if (children.length === 1) {
    return [span(parent.offset + 1, end(parent) - 1)];
  }

  // What may be touched: never into the neighbour before, never into the neighbour after.
  const lower = at > 0 ? end(children[at - 1]) : parent.offset + 1;
  const last = at === children.length - 1;
  const upper = last ? end(parent) - 1 : children[at + 1].offset;

  const owned = ownedLines(source, item, lower, upper);
  if (owned === undefined) {
    // Sharing its line: the span from its neighbour's comma, or up to its neighbour's start.
    return [last ? span(lower, end(item)) : span(item.offset, upper)];
  }

  // Its own comma sits on its own last line and goes with it — unless it is the last of them, and
  // the comma that joined it belongs to the line above.
  const comma = last ? commaIn(source, lower, owned.from) : undefined;

  return [
    ...(comma === undefined ? [] : [span(comma, comma + 1)]),
    span(owned.from, owned.to),
  ];
}

/**
 * The lines this item has to itself: from the start of its own line, walked back over the comment
 * lines and blank lines above it that no neighbour is written on, to the end of the line its last
 * character is on. Undefined where a neighbour shares either line, which leaves it no lines of
 * its own.
 */
function ownedLines(
  source: string,
  item: Node,
  lower: number,
  upper: number,
): { from: number; to: number } | undefined {
  let from = lineStart(source, item.offset);
  const to = lineAfter(source, end(item));

  if (from < lower || to > upper || source.slice(from, item.offset).trim() !== '') {
    return undefined;
  }

  while (from > 0) {
    const above = lineStart(source, from - 1);
    const line = source.slice(above, from);

    if (above < lower || !(line.trim() === '' || COMMENT_LINE.test(line))) {
      break;
    }

    from = above;
  }

  return { from, to };
}

function span(from: number, to: number): TextChange {
  return { offset: from, length: to - from, content: '' };
}

/** The start of the line the offset is on. */
function lineStart(source: string, offset: number): number {
  return source.lastIndexOf('\n', Math.max(offset - 1, 0)) + 1;
}

/** Just past the end of the line the offset is on, the line break included. */
function lineAfter(source: string, offset: number): number {
  const at = source.indexOf('\n', offset);
  return at === -1 ? source.length : at + 1;
}

/** The comma joining the neighbour above, which is whatever sits between the two of them. */
function commaIn(source: string, from: number, to: number): number | undefined {
  const at = source.indexOf(',', from);
  return at === -1 || at >= to ? undefined : at;
}

function end(node: Node): number {
  return node.offset + node.length;
}

function spanOf(change: TextChange): TextChange {
  return { offset: change.offset, length: change.length, content: change.content };
}

/**
 * The changes that change something. Putting back what is already there is not a change: a box
 * left as it was found, a dropdown re-picking the value it had. Written anyway it would mark the
 * file dirty for nothing, and a file marked dirty for nothing is a file nobody trusts the mark on.
 */
function real(source: string, changes: readonly TextChange[]): readonly TextChange[] {
  return changes.filter(
    (change) => source.slice(change.offset, change.offset + change.length) !== change.content,
  );
}

/** Whether the write only puts something over what the file already holds at that very place. */
function replacing(source: string, write: Write): boolean {
  if (write.value === undefined || write.appending) {
    return false;
  }

  const root = parseTree(source, [], PARSING);

  return root !== undefined && findNodeAtLocation(root, [...write.path]) !== undefined;
}

/** A key answered for twice, which is a file the form would show one of and write into the other. */
function repeatedIn(node: Node | undefined): string | undefined {
  if (node === undefined) {
    return undefined;
  }

  if (node.type === 'object') {
    const keys = (node.children ?? []).flatMap((property) => {
      const key = property.children?.[0];
      return key === undefined ? [] : [textOf(key)];
    });

    const twice = keys.find((key, at) => keys.indexOf(key) !== at);
    if (twice !== undefined) {
      return twice;
    }
  }

  return (node.children ?? []).map(repeatedIn).find((found) => found !== undefined);
}

/**
 * The first list the form shows fewer rows of than the file writes items. The reader drops what it
 * could not read — a number among the masks, a target with no name — and every row after the gap
 * then sits one lower in the file than it looks, so the rows are no longer addresses.
 */
function droppedFrom(
  kind: ManifestKind,
  root: Node | undefined,
  source: string,
): string | undefined {
  if (root === undefined) {
    return undefined;
  }

  const read = readOf(kind, source);
  const shown = new Map<string, number>([
    ['exclude', read.mod?.exclude.length ?? 0],
    [`${LAUNCH}.clientMods`, read.launch.clientMods.length],
    [`${LAUNCH}.serverMods`, read.launch.serverMods.length],
    [`${LAUNCH}.${TARGETS}`, read.launch.targets.length],
  ]);

  return (kind === 'mod' ? MOD_LISTS : WORKSPACE_LISTS)
    .map((path) => ({ name: path.join('.'), node: findNodeAtLocation(root, [...path]) }))
    .find(
      ({ name, node }) =>
        node?.type === 'array' && (node.children ?? []).length !== shown.get(name),
    )?.name;
}

/**
 * How this file is written, so that what the form adds is written the same way. Only the lines a
 * change touches are formatted, so getting this wrong shows up as one line indented unlike its
 * neighbours rather than as a file reformatted whole — which is worse, being invisible in review.
 */
function formattingOf(source: string): FormattingOptions {
  const indent = INDENT.exec(source)?.[1] ?? '';
  const tabbed = indent.startsWith('\t');

  return {
    tabSize: tabbed || indent === '' ? INDENT_WIDTH : indent.length,
    insertSpaces: !tabbed,
    // A file written on Windows is CRLF throughout, and a line inserted as LF is a line every
    // machine that touches the file afterwards shows as changed.
    eol: source.includes('\r\n') ? '\r\n' : '\n',
  };
}

/**
 * Where a field being written goes among the ones the block already holds, for the parser to ask
 * as it writes. It is asked once per block it has to make on the way, so writing into a `launch`
 * that is not there yet asks this about the root as well — with the launch block's field order,
 * which knows none of the root's fields and so says "at the end". Which is where `launch` goes in
 * both files, and a test holds it there.
 */
function insertionIndexOf(
  kind: ManifestKind,
  path: FormPath,
): ((properties: string[]) => number) | undefined {
  const field = path.at(-1);
  const order = orderOf(kind, path);

  if (typeof field !== 'string' || order === undefined) {
    return undefined;
  }

  return (properties) => placeOf(order, field, properties);
}

/** The block the path ends in, or undefined where it ends in a list, which has no field order. */
function orderOf(kind: ManifestKind, path: FormPath): readonly string[] | undefined {
  if (typeof path.at(-1) !== 'string') {
    return undefined;
  }

  const parent = path.slice(0, -1);
  if (parent.length === 0) {
    return kind === 'mod' ? MOD_FIELDS : WORKSPACE_FIELDS;
  }
  if (parent.length === 1 && parent[0] === LAUNCH) {
    return LAUNCH_FIELDS;
  }
  if (parent.length === 3 && parent[0] === LAUNCH && parent[1] === TARGETS) {
    return TARGET_FIELDS;
  }

  return undefined;
}

/**
 * Where a field being added goes among the ones already there: after every field the schema writes
 * before it, and before the first it writes after. A key nobody declared is passed over rather
 * than counted, so a typo at the end of the file does not drag the new fields down to it.
 */
function placeOf(order: readonly string[], field: string, existing: readonly string[]): number {
  const rank = order.indexOf(field);
  if (rank === -1) {
    return existing.length;
  }

  const at = existing.findIndex((key) => {
    const other = order.indexOf(key);
    return other !== -1 && other > rank;
  });

  return at === -1 ? existing.length : at;
}

/** `Node.value` is `any`, and this is the one place that ends. */
function textOf(node: Node): string {
  return typeof node.value === 'string' ? node.value : '';
}
