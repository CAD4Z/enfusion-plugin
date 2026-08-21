/**
 * Reading a `config.cpp`.
 *
 * Two things are asked of it for the model: which addons a `CfgPatches` class requires, so the
 * build order can be worked out, and whether the file carries a `CfgMods` block, because that is
 * what makes an addon the main one and names the mod's prefix root. What a mod says about itself
 * there — its name, who wrote it, what it does — is read as well, because a mod already carrying
 * all of that is a mod nobody should be asked to type it into a `mod.enf` a second time.
 *
 * The format is Arma's class syntax, so the parser has to cope with what real configs are written
 * with: `#include` lines, both comment styles, and `""` as an escaped quote. It never throws — a
 * config nobody can parse still has to leave the mod visible in the panel.
 *
 * The one thing written back is a name in a `requiredAddons` list, which is what a new addon is
 * added to a mod with. It is an edit rather than a rewrite: the parser keeps where every value it
 * read sits, so the comments, the order of the members and the way a list is laid out all survive
 * an addon being written into the file.
 */

/** What a `config.cpp` says about the addon it declares. */
export interface ConfigCpp {
  /** The `CfgPatches` classes, in file order. Their names are what `requiredAddons` refers to. */
  readonly patches: readonly PatchClass[];
  /** The `CfgMods` declaration, which only the main addon of a mod carries. */
  readonly mod?: ModClass;
}

/** One `CfgPatches` class: an addon name other addons can require. */
export interface PatchClass {
  readonly name: string;
  readonly requiredAddons: readonly string[];
  /** Who wrote the addon; an Arma habit that plenty of DayZ configs keep, and plenty leave out. */
  readonly author: string | undefined;
  /** The addon's own version — not `requiredVersion`, which is the game's. */
  readonly version: string | undefined;
}

/** The `CfgMods` class that declares the mod itself; carrying one is what makes an addon main. */
export interface ModClass {
  /** The prefix root's folder name — `P:\<dir>` — undefined when the config leaves it out. */
  readonly dir: string | undefined;
  /** What the mod calls itself, which is neither the class name nor the folder. */
  readonly name: string | undefined;
  /** What it does, in a sentence: `overview` is where a DayZ config writes that. */
  readonly overview: string | undefined;
  /** Who made the mod, as the mod itself has it rather than as one of its addons does. */
  readonly author: string | undefined;
  /** Which version of the mod this is — the mod's own, and nothing to do with the game's. */
  readonly version: string | undefined;
}

/** Everything the parser could make sense of; anything malformed is skipped, not reported. */
export function parseConfig(source: string): ConfigCpp {
  const root = parse(source);

  const patches = childrenOf(root, 'CfgPatches').map((patch) => ({
    name: patch.name,
    requiredAddons: valuesOf(patch, 'requiredAddons'),
    author: scalarOf(patch, 'author'),
    version: scalarOf(patch, 'version'),
  }));

  // A second class under `CfgMods` would be a second mod in one addon, which the engine has no
  // notion of; the first one is the declaration.
  const declaration = childrenOf(root, 'CfgMods')[0];
  const mod = declaration && {
    dir: scalarOf(declaration, 'dir'),
    name: scalarOf(declaration, 'name'),
    overview: scalarOf(declaration, 'overview'),
    author: scalarOf(declaration, 'author'),
    version: scalarOf(declaration, 'version'),
  };

  return { patches, mod };
}

/**
 * A class body. Every entry is kept as a list of strings, whether it was written as an array or as
 * a scalar, so that reading one takes the same shape either way. Keys are lowercased because the
 * engine matches them case-insensitively.
 */
interface Body {
  readonly classes: ClassNode[];
  readonly entries: Map<string, Entry>;
  /** What sits between the braces, which is where a member is written into the body. */
  within: Span;
}

interface ClassNode extends Body {
  readonly name: string;
}

/** One `key = value;`: what it says, and where each part of it says it. */
interface Entry {
  readonly values: string[];
  /** An array's insides, between its braces; anything else's own extent. */
  readonly within: Span;
  /** Where each value sits, so a list can be written to after its last one rather than after
   * whatever character comes last — which, in a list with a comment in it, is the comment. */
  readonly items: readonly Span[];
}

/** A stretch of the source, from `start` up to but not including `end`. */
interface Span {
  readonly start: number;
  readonly end: number;
}

/** Where the parser is in the source. */
interface Cursor {
  readonly text: string;
  at: number;
}

/** The direct children of every top-level class with this name. */
function childrenOf(root: Body, name: string): ClassNode[] {
  return root.classes.filter((node) => sameName(node.name, name)).flatMap((node) => node.classes);
}

/** The entry's values, empty when it is not there. */
function valuesOf(node: Body, key: string): string[] {
  return node.entries.get(key.toLowerCase())?.values ?? [];
}

/**
 * The entry's one value, and undefined where it is written empty as well as where it is absent:
 * `picture = ""` and `author = ""` are half of what a config copied from a template holds, and a
 * field that says nothing is a field that was never filled in.
 */
function scalarOf(node: Body, key: string): string | undefined {
  const value = valuesOf(node, key)[0]?.trim();
  return value === undefined || value === '' ? undefined : value;
}

/** Names are matched the way the engine matches them: without regard for case. */
export function sameName(a: string, b: string): boolean {
  return a.toLowerCase() === b.toLowerCase();
}

function parse(text: string): Body {
  const root = emptyBody(0);
  parseBody({ text, at: 0 }, root);
  return root;
}

function emptyBody(start: number): Body {
  return { classes: [], entries: new Map(), within: { start, end: start } };
}

/** Members until the closing brace or the end of the source, whichever comes first. */
function parseBody(cursor: Cursor, body: Body): void {
  for (;;) {
    skipTrivia(cursor);

    const char = cursor.text[cursor.at];
    if (char === undefined || char === '}') {
      // The body ends at its closing brace, or wherever the source ran out before one.
      body.within = { start: body.within.start, end: Math.min(cursor.at, cursor.text.length) };
      cursor.at += 1;
      return;
    }

    const word = readWord(cursor);
    if (word === '') {
      // Nothing a member can start with: step over it and carry on rather than give up on the file.
      cursor.at += 1;
      continue;
    }

    if (sameName(word, 'class')) {
      parseClass(cursor, body);
    } else {
      parseEntry(cursor, word, body);
    }
  }
}

/** `class Name : Parent { ... };`, or a forward declaration with no body at all. */
function parseClass(cursor: Cursor, body: Body): void {
  skipTrivia(cursor);
  const name = readWord(cursor);

  skipTrivia(cursor);
  if (cursor.text[cursor.at] === ':') {
    cursor.at += 1;
    skipTrivia(cursor);
    // The parent is only read to get past it: nothing here follows inherited members.
    readWord(cursor);
    skipTrivia(cursor);
  }

  if (cursor.text[cursor.at] !== '{') {
    return;
  }

  cursor.at += 1;
  const node: ClassNode = { name, ...emptyBody(cursor.at) };
  body.classes.push(node);
  parseBody(cursor, node);
}

/** `key = value;` and `key[] = { ... };`, both of which land as a list of strings. */
function parseEntry(cursor: Cursor, key: string, body: Body): void {
  skipTrivia(cursor);

  if (cursor.text[cursor.at] === '[') {
    cursor.at += 1;
    skipTrivia(cursor);
    if (cursor.text[cursor.at] === ']') {
      cursor.at += 1;
      skipTrivia(cursor);
    }
  }

  // `+=` appends to an inherited array; for reading the result it is the same as `=`.
  if (cursor.text[cursor.at] === '+') {
    cursor.at += 1;
    skipTrivia(cursor);
  }

  if (cursor.text[cursor.at] !== '=') {
    return;
  }

  cursor.at += 1;
  body.entries.set(key.toLowerCase(), readEntry(cursor));
}

/** The value, and the stretch of source it was read out of. */
function readEntry(cursor: Cursor): Entry {
  skipTrivia(cursor);

  if (cursor.text[cursor.at] === '{') {
    cursor.at += 1;
    const start = cursor.at;
    const array = readArray(cursor);

    // `readArray` stops past the closing brace, or past the end of the source.
    return {
      values: array.values,
      within: { start, end: Math.min(cursor.at - 1, cursor.text.length) },
      items: array.items,
    };
  }

  const start = cursor.at;
  const values = cursor.text[cursor.at] === '"' ? [readString(cursor)] : [readRaw(cursor)];

  return { values, within: { start, end: cursor.at }, items: [{ start, end: cursor.at }] };
}

/** What an array holds, and where each of it sits. */
interface ArrayItems {
  readonly values: string[];
  readonly items: Span[];
}

/** Items until the closing brace. Nested arrays are flattened: no entry read here has a shape. */
function readArray(cursor: Cursor): ArrayItems {
  const values: string[] = [];
  const items: Span[] = [];

  for (;;) {
    skipTrivia(cursor);

    const start = cursor.at;
    const char = cursor.text[cursor.at];
    if (char === undefined || char === '}') {
      cursor.at += 1;
      return { values, items };
    }

    if (char === ',') {
      cursor.at += 1;
    } else if (char === '{') {
      cursor.at += 1;
      const nested = readArray(cursor);
      values.push(...nested.values);
      items.push(...nested.items);
    } else if (char === '"') {
      values.push(readString(cursor));
      items.push({ start, end: cursor.at });
    } else {
      const raw = readRaw(cursor);
      if (raw === '') {
        // Nothing consumable here, so step over it rather than spin on the same character.
        cursor.at += 1;
      } else {
        values.push(raw);
        items.push({ start, end: cursor.at });
      }
    }
  }
}

/** A quoted string, in which `""` stands for one quote. */
function readString(cursor: Cursor): string {
  cursor.at += 1;
  let value = '';

  for (;;) {
    const char = cursor.text[cursor.at];
    if (char === undefined) {
      return value;
    }

    cursor.at += 1;
    if (char !== '"') {
      value += char;
      continue;
    }

    if (cursor.text[cursor.at] !== '"') {
      return value;
    }

    cursor.at += 1;
    value += '"';
  }
}

/** An unquoted value — a number, a macro, an identifier — up to whatever ends it. */
function readRaw(cursor: Cursor): string {
  const start = cursor.at;

  while (cursor.at < cursor.text.length && !ENDS_RAW.has(cursor.text[cursor.at])) {
    cursor.at += 1;
  }

  return cursor.text.slice(start, cursor.at).trim();
}

const ENDS_RAW = new Set([';', ',', '}', '{', '\r', '\n']);

function readWord(cursor: Cursor): string {
  WORD.lastIndex = cursor.at;
  const word = WORD.exec(cursor.text)?.[0] ?? '';
  cursor.at += word.length;
  return word;
}

const WORD = /[A-Za-z_][A-Za-z0-9_]*/y;

/** Whitespace, both comment styles, and preprocessor lines, which this parser has no use for. */
function skipTrivia(cursor: Cursor): void {
  for (;;) {
    const char = cursor.text[cursor.at];

    if (char === undefined) {
      return;
    }

    if (/\s/.test(char)) {
      cursor.at += 1;
      continue;
    }

    if (char === '#') {
      skipDirective(cursor);
      continue;
    }

    if (char !== '/') {
      return;
    }

    const next = cursor.text[cursor.at + 1];
    if (next === '/') {
      skipTo(cursor, '\n');
    } else if (next === '*') {
      cursor.at += 2;
      skipPast(cursor, '*/');
    } else {
      return;
    }
  }
}

/** A directive runs to the end of the line, unless the line is continued with a backslash. */
function skipDirective(cursor: Cursor): void {
  for (;;) {
    skipTo(cursor, '\n');

    let end = cursor.at;
    while (end > 0 && TRAILING.has(cursor.text[end - 1])) {
      end -= 1;
    }

    if (cursor.text[end - 1] !== '\\') {
      return;
    }

    cursor.at += 1;
  }
}

const TRAILING = new Set([' ', '\t', '\r']);

function skipTo(cursor: Cursor, char: string): void {
  const found = cursor.text.indexOf(char, cursor.at);
  cursor.at = found === -1 ? cursor.text.length : found;
}

function skipPast(cursor: Cursor, end: string): void {
  const found = cursor.text.indexOf(end, cursor.at);
  cursor.at = found === -1 ? cursor.text.length : found + end.length;
}

/**
 * The source with one more name in the `requiredAddons` of a `CfgPatches` class, or undefined when
 * the file holds no such class. An addon that nothing requires is one the engine is free to load
 * in whatever order it likes, which is how a new addon of a mod goes missing without a word.
 *
 * The file is edited rather than rewritten: the one name is written where it belongs and
 * everything else — the comments, the order of the members, the way the list is laid out — is
 * left exactly as the developer wrote it.
 */
export function withRequiredAddon(
  source: string,
  patch: string,
  required: string,
): string | undefined {
  const node = childrenOf(parse(source), 'CfgPatches').find((child) => sameName(child.name, patch));
  if (node === undefined) {
    return undefined;
  }

  const entry = node.entries.get(REQUIRED_ADDONS);
  if (entry === undefined) {
    return withMember(source, node.within, `requiredAddons[] = { "${required}" };`);
  }

  // Required already — under another spelling of the same name, as often as not — is nothing to do.
  if (entry.values.some((value) => sameName(value, required))) {
    return source;
  }

  return withItem(source, entry, `"${required}"`);
}

const REQUIRED_ADDONS = 'requiredaddons';

/**
 * One more item at the end of a list, written the way the list is already written.
 *
 * It goes in right after the last value rather than at the end of what is between the braces,
 * because the two are not the same thing: a list with a comment after its last item would take the
 * separating comma into the comment, and one holding nothing but a comment would come out starting
 * with one.
 */
function withItem(source: string, entry: Entry, item: string): string {
  const last = entry.items.at(-1);

  // A list of no values is filled in rather than added to, whatever else is written between its
  // braces: `{ , "X" }` is not a list, and a comment in there is not an item to follow.
  if (last === undefined) {
    const inner = source.slice(entry.within.start, entry.within.end);
    const written = inner.replace(TRAILING_SPACE, '');
    const tail = inner.slice(written.length);

    const before = written === '' ? ' ' : `${written} `;

    return splice(source, entry.within, `${before}${item}${tail === '' ? ' ' : tail}`);
  }

  const after = source.slice(last.end, entry.within.end);
  const comma = after.trimStart().startsWith(',') ? '' : ',';

  // A list written a line at a time gets another line; one written along a line gets another item.
  const added = after.includes('\n')
    ? `${comma}${newlineOf(source)}${indentAt(source, last.start)}${item}`
    : `${comma} ${item}`;

  return splice(source, { start: last.end, end: last.end }, added);
}

/** One more member at the end of a class body, indented the way its members are indented. */
function withMember(source: string, within: Span, member: string): string {
  const inner = source.slice(within.start, within.end);
  const written = inner.replace(TRAILING_SPACE, '');
  const tail = inner.slice(written.length);
  const added = tail.includes('\n')
    ? `${newlineOf(source)}${indentAt(source, within.start + written.length)}${member}`
    : ` ${member}`;

  return splice(source, within, `${written}${added}${tail}`);
}

const TRAILING_SPACE = /\s+$/;

/**
 * How this file ends its lines, so that a line written into it ends the way the rest do. A config
 * written on Windows — which is most of them — is `\r\n` throughout, and one line of `\n` in it
 * shows up as the whole file having changed.
 */
function newlineOf(source: string): string {
  return source.includes('\r\n') ? '\r\n' : '\n';
}

/** What the line holding this offset is indented by, which is what a line under it takes. */
function indentAt(source: string, at: number): string {
  // Up to `at` and not including it, because `at` itself can be the newline that ends the line.
  const line = source.slice(source.lastIndexOf('\n', at - 1) + 1, at);

  return /^[ \t]*/.exec(line)?.[0] ?? '';
}

function splice(source: string, within: Span, text: string): string {
  return source.slice(0, within.start) + text + source.slice(within.end);
}
