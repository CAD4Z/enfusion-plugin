/**
 * Holds the text of the repository to what a public repository must not contain, and the native C
 * sources to the layout rules that clang-format does not keep.
 *
 * Every text file:
 * - no Cyrillic, no control character but tab and line breaks, no invisible character (soft
 *   hyphen, zero-width marks, a byte order mark) and nothing that is not UTF-8: a script edit
 *   leaves such characters behind unseen;
 * - no path of a developer's machine, no reference to an issue tracker or to a document that lives
 *   outside the repository, and no name of the projects this extension is developed beside;
 * - none of the workspace documents that .gitignore keeps out is tracked;
 * - nothing is left behind: the top of the repository holds only what TOP_LEVEL names, and no
 *   folder holds what an editor, the system, a build or a merge leaves (LEFTOVER).
 *
 * The C sources under native/ also: ASCII only, no tab, no trailing space, no trigraph, lines of
 * code up to 140 columns and lines of comment up to 100.
 *
 * Words a clone keeps out without naming them here go into .git/info/check-text-words, one regular
 * expression per line, matched regardless of case; a line starting with # is a comment.
 *
 *   node scripts/check-text.mjs            every tracked file and every new one git does not ignore,
 *                                          as it is in the working tree
 *   node scripts/check-text.mjs --staged   every staged file, as it is staged (the pre-commit hook)
 */

import { execFileSync } from 'node:child_process';
import fs from 'node:fs';
import process from 'node:process';
import { TextDecoder } from 'node:util';

/** Workspace documents that sit beside the repository on disk and stay out of it. */
const WORKSPACE_DOCUMENT = /^(?:CONTEXT\.md|CODING_STANDARDS\.md|docs\/|\.claude\/)/;

/**
 * What the top of the repository holds. Anything else there was left behind — a probe, a note, a
 * built package — and a file that does belong at the top is added here.
 */
const TOP_LEVEL = new Set([
  '.editorconfig',
  '.gitattributes',
  '.githooks',
  '.github',
  '.gitignore',
  '.vscode',
  '.vscodeignore',
  'LICENSE',
  'MANUAL.md',
  'README.md',
  'THIRD-PARTY.md',
  'esbuild.js',
  'eslint.config.mjs',
  'native',
  'package-lock.json',
  'package.json',
  'resources',
  'schemas',
  'scripts',
  'src',
  'test',
  'tsconfig.json',
]);

/** What an editor, the system, a build or a merge leaves in a folder: no folder keeps it. */
const LEFTOVER =
  /(?:^|\/)(?:Thumbs\.db|desktop\.ini|\.DS_Store|__pycache__\/.*|[^/]*~)$|\.(?:log|tmp|bak|orig|rej|swp|pyc|vsix|zip|7z|exe|dll|pdb|obj|ilk|exp|lib)$/i;

/** Files that are not text, by extension. */
const BINARY = /\.(?:png|jpe?g|gif|ico|bmp|tga|tiff?|dds|edds|hdr|exr|ttf|otf|woff2?|vsix|zip|gz|7z|exe|dll|pdb|pbo|bin)$/i;

/** The native C sources, which are held to the layout rules as well. */
const NATIVE_C = /^native\/.+\.[ch]$/;

/** Invisible characters: soft hyphen, zero-width and directional marks, separators, joiners, BOM. */
const INVISIBLE = new Set([0xad, 0x200b, 0x200c, 0x200d, 0x200e, 0x200f, 0x2028, 0x2029, 0x2060, 0x2061, 0x2062, 0x2063, 0x2064, 0xfeff]);

/**
 * The words no line of a text file may hold. A letter in brackets keeps this file from matching
 * its own patterns; `allow` is cut out of a line before the pattern is tried, and `exempt` names
 * the files a rule does not apply to.
 *
 * @type {{ name: string, pattern: RegExp, allow?: RegExp, exempt?: string[] }[]}
 */
const WORD_RULES = [
  { name: 'Cyrillic', pattern: /\p{Script=Cyrillic}+/u },
  // A user profile in an example path is `dev`; any other name there is somebody's machine.
  { name: 'machine path', pattern: /\b[A-Za-z]:[\\/]+[U]sers[\\/]+(?!dev\b)[^\\/\s'"`]+/i },
  { name: 'tracker reference', pattern: /\b[T]icket\s+\d+|\bissues\/\d{2}-|\.s[c]ratch\b|\bdocs\/a[d]r\b/i },
  {
    name: 'project name',
    pattern: /\bC[A]D(?:4Z\b|_[A-Za-z]|[A-Z][a-z])|\bc[a]d4z\b|\bC[A]D\b/,
    // The organisation the repository is published under, as GitHub names it: @org, org/repo, /org.
    allow: /[/@]C[A]D4Z\b|\bC[A]D4Z\//g,
    // The copyright lines.
    exempt: ['LICENSE', 'native/NOTICE.txt'],
  },
];

/** Code lines of the native sources may run to this column, comment lines to the next one. */
const NATIVE_CODE_COLUMNS = 140;
const NATIVE_COMMENT_COLUMNS = 100;

/** @typedef {{ path: string, line: number, problem: string }} Finding */

function main() {
  const staged = process.argv.includes('--staged');
  process.chdir(git(['rev-parse', '--show-toplevel']).trim());

  const paths = staged
    ? git(['diff', '--cached', '--name-only', '-z', '--diff-filter=ACMR']).split('\0').filter(Boolean)
    : git(['ls-files', '-z', '--cached', '--others', '--exclude-standard']).split('\0').filter(Boolean);
  const privateRules = privateWordRules();

  /** @type {Finding[]} */
  const findings = [];
  let checked = 0;

  for (const path of paths) {
    // A workspace document is wrong to track whatever it says.
    if (WORKSPACE_DOCUMENT.test(path)) {
      findings.push({ path, line: 0, problem: 'workspace document: it stays out of the repository (see .gitignore)' });
      continue;
    }

    // So is a file nothing in the repository asked for.
    if (!TOP_LEVEL.has(path.split('/')[0] ?? '')) {
      findings.push({ path, line: 0, problem: 'not one of the files the top of the repository holds (TOP_LEVEL)' });
      continue;
    }
    if (LEFTOVER.test(path)) {
      findings.push({ path, line: 0, problem: 'left behind by an editor, the system, a build or a merge' });
      continue;
    }

    if (BINARY.test(path)) {
      continue;
    }

    const bytes = staged ? stagedBytes(path) : workingBytes(path);
    if (bytes === undefined) {
      continue;
    }

    checked += 1;
    findings.push(...fileFindings(path, bytes, privateRules));
  }

  for (const { path, line, problem } of findings) {
    process.stdout.write(`${path}${line > 0 ? `:${line}` : ''}: ${problem}\n`);
  }

  if (findings.length > 0) {
    const files = new Set(findings.map((finding) => finding.path)).size;
    process.stderr.write(`check-text: ${findings.length} problem(s) in ${files} file(s)\n`);
    process.exitCode = 1;
    return;
  }

  process.stdout.write(`check-text: ${checked} ${staged ? 'staged ' : ''}file(s) clean\n`);
}

/**
 * The problems of one text file.
 *
 * @param {string} path
 * @param {Buffer} bytes
 * @param {{ name: string, pattern: RegExp }[]} privateRules
 * @returns {Finding[]}
 */
function fileFindings(path, bytes, privateRules) {
  /** @type {Finding[]} */
  const findings = [];

  let text;
  try {
    text = new TextDecoder('utf-8', { fatal: true }).decode(bytes);
  } catch {
    return [{ path, line: 0, problem: 'not UTF-8' }];
  }

  const lines = text.split('\n').map((line) => (line.endsWith('\r') ? line.slice(0, -1) : line));
  const rules = [...WORD_RULES.filter((rule) => !(rule.exempt ?? []).includes(path)), ...privateRules];

  lines.forEach((line, index) => {
    const stray = strayCharacter(line);
    if (stray !== undefined) {
      findings.push({ path, line: index + 1, problem: stray });
    }

    for (const rule of rules) {
      const tried = 'allow' in rule && rule.allow !== undefined ? line.replace(rule.allow, '') : line;
      const match = rule.pattern.exec(tried);
      if (match !== null) {
        findings.push({ path, line: index + 1, problem: `${rule.name}: "${match[0]}"` });
      }
    }
  });

  if (NATIVE_C.test(path)) {
    findings.push(...nativeFindings(path, lines));
  }

  return findings;
}

/**
 * The layout problems of a native C source. A line is a comment line when it starts a comment or
 * sits inside a block comment; a comment after code on the same line counts as code.
 *
 * @param {string} path
 * @param {string[]} lines
 * @returns {Finding[]}
 */
function nativeFindings(path, lines) {
  /** @type {Finding[]} */
  const findings = [];
  let inBlockComment = false;

  lines.forEach((line, index) => {
    const trimmed = line.trimStart();
    const isComment = inBlockComment || trimmed.startsWith('//') || trimmed.startsWith('/*');

    // A block comment that opens on this line and does not close on it goes on to the next.
    if (inBlockComment) {
      inBlockComment = !line.includes('*/');
    } else if (trimmed.startsWith('/*')) {
      inBlockComment = !trimmed.slice(2).includes('*/');
    }

    /** @param {string} problem */
    const report = (problem) => findings.push({ path, line: index + 1, problem });

    if (/[\u0080-\u{10FFFF}]/u.test(line)) {
      report('non-ASCII character');
    }
    if (line.includes('\t')) {
      report('tab');
    }
    if (/[ \t]+$/.test(line)) {
      report('trailing space');
    }
    if (/\?\?[=/'()!<>-]/.test(line)) {
      report('trigraph');
    }

    if (line.length > NATIVE_CODE_COLUMNS) {
      report(`${line.length} columns, code keeps to ${NATIVE_CODE_COLUMNS}`);
    } else if (isComment && line.length > NATIVE_COMMENT_COLUMNS) {
      report(`${line.length} columns, a comment keeps to ${NATIVE_COMMENT_COLUMNS}`);
    }
  });

  return findings;
}

/**
 * The first control character (other than tab and carriage return) or invisible character of a
 * line, named with its code point.
 *
 * @param {string} line
 * @returns {string | undefined}
 */
function strayCharacter(line) {
  for (const character of line) {
    const code = character.codePointAt(0) ?? 0;

    if ((code < 0x20 && code !== 0x09 && code !== 0x0d) || code === 0x7f) {
      return `control character U+${hex(code)}`;
    }
    if (INVISIBLE.has(code)) {
      return `invisible character U+${hex(code)}`;
    }
  }

  return undefined;
}

/**
 * The rules of .git/info/check-text-words: words this clone keeps out of the repository.
 *
 * @returns {{ name: string, pattern: RegExp }[]}
 */
function privateWordRules() {
  const file = git(['rev-parse', '--git-path', 'info/check-text-words']).trim();
  if (!fs.existsSync(file)) {
    return [];
  }

  return fs
    .readFileSync(file, 'utf8')
    .split(/\r?\n/)
    .map((line) => line.trim())
    .filter((line) => line !== '' && !line.startsWith('#'))
    .map((source) => ({ name: 'private word', pattern: new RegExp(source, 'iu') }));
}

/**
 * A staged file's bytes, as the commit will hold them.
 *
 * @param {string} path
 * @returns {Buffer}
 */
function stagedBytes(path) {
  return execFileSync('git', ['show', `:${path}`], { maxBuffer: 1 << 30 });
}

/**
 * A tracked file's bytes in the working tree, or nothing when it has been deleted there.
 *
 * @param {string} path
 * @returns {Buffer | undefined}
 */
function workingBytes(path) {
  return fs.existsSync(path) ? fs.readFileSync(path) : undefined;
}

/**
 * Runs git and returns what it printed.
 *
 * @param {string[]} args
 * @returns {string}
 */
function git(args) {
  return execFileSync('git', args, { encoding: 'utf8', maxBuffer: 1 << 30 });
}

/** @param {number} code */
function hex(code) {
  return code.toString(16).toUpperCase().padStart(4, '0');
}

main();
