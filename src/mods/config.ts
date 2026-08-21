/**
 * Reading a `config.cpp`.
 *
 * Only two things are asked of it: which addons a `CfgPatches` class requires, so the build order
 * can be worked out, and whether the file carries a `CfgMods` block, because that is what makes an
 * addon the main one and names the mod's prefix root.
 *
 * The format is Arma's class syntax, so the parser has to cope with what real configs are written
 * with: `#include` lines, both comment styles, and `""` as an escaped quote. It never throws — a
 * config nobody can parse still has to leave the mod visible in the panel.
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
}

/** The `CfgMods` class that declares the mod itself; carrying one is what makes an addon main. */
export interface ModClass {
  /** The prefix root's folder name — `P:\<dir>` — empty when the config leaves it out. */
  readonly dir: string;
}

/** Everything the parser could make sense of; anything malformed is skipped, not reported. */
export function parseConfig(source: string): ConfigCpp {
  const root = parse(source);

  const patches = childrenOf(root, 'CfgPatches').map((patch) => ({
    name: patch.name,
    requiredAddons: valuesOf(patch, 'requiredAddons'),
  }));

  // A second class under `CfgMods` would be a second mod in one addon, which the engine has no
  // notion of; the first one is the declaration.
  const declaration = childrenOf(root, 'CfgMods')[0];
  const mod = declaration && { dir: valuesOf(declaration, 'dir')[0] ?? '' };

  return { patches, mod };
}

/**
 * A class body. Every entry is kept as a list of strings, whether it was written as an array or as
 * a scalar, so that reading one takes the same shape either way. Keys are lowercased because the
 * engine matches them case-insensitively.
 */
interface Body {
  readonly classes: ClassNode[];
  readonly entries: Map<string, string[]>;
}

interface ClassNode extends Body {
  readonly name: string;
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
  return node.entries.get(key.toLowerCase()) ?? [];
}

/** Names are matched the way the engine matches them: without regard for case. */
export function sameName(a: string, b: string): boolean {
  return a.toLowerCase() === b.toLowerCase();
}

function parse(text: string): Body {
  const root = emptyBody();
  parseBody({ text, at: 0 }, root);
  return root;
}

function emptyBody(): Body {
  return { classes: [], entries: new Map() };
}

/** Members until the closing brace or the end of the source, whichever comes first. */
function parseBody(cursor: Cursor, body: Body): void {
  for (;;) {
    skipTrivia(cursor);

    const char = cursor.text[cursor.at];
    if (char === undefined || char === '}') {
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
  const node: ClassNode = { name, ...emptyBody() };
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
  body.entries.set(key.toLowerCase(), readValue(cursor));
}

function readValue(cursor: Cursor): string[] {
  skipTrivia(cursor);

  if (cursor.text[cursor.at] === '{') {
    cursor.at += 1;
    return readArray(cursor);
  }

  if (cursor.text[cursor.at] === '"') {
    return [readString(cursor)];
  }

  return [readRaw(cursor)];
}

/** Items until the closing brace. Nested arrays are flattened: no entry read here has a shape. */
function readArray(cursor: Cursor): string[] {
  const items: string[] = [];

  for (;;) {
    skipTrivia(cursor);

    const char = cursor.text[cursor.at];
    if (char === undefined || char === '}') {
      cursor.at += 1;
      return items;
    }

    if (char === ',') {
      cursor.at += 1;
    } else if (char === '{') {
      cursor.at += 1;
      items.push(...readArray(cursor));
    } else if (char === '"') {
      items.push(readString(cursor));
    } else {
      const raw = readRaw(cursor);
      if (raw === '') {
        // Nothing consumable here, so step over it rather than spin on the same character.
        cursor.at += 1;
      } else {
        items.push(raw);
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
