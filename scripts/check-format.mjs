/**
 * Holds the native C sources to native/.clang-format and the native Python tests to
 * native/tests/ruff.toml: the layout each formatter keeps, checked without rewriting anything.
 *
 *   node scripts/check-format.mjs            every C and Python file, tracked or new and not ignored
 *   node scripts/check-format.mjs --staged   the staged ones, as they are in the working tree
 *
 * clang-format 20 or newer is taken from CLANG_FORMAT, PATH or the default LLVM folder of Windows;
 * ruff from RUFF, PATH, or as a module of py -3 or python3 (native/tests/requirements.txt pins it).
 * A missing tool fails the check, except under --staged, which says so and goes on: CI runs both.
 */

import { execFileSync, spawnSync } from 'node:child_process';
import fs from 'node:fs';
import process from 'node:process';

/** The files each formatter keeps. */
const C_SOURCE = /^native\/.+\.[ch]$/;
const PYTHON_SOURCE = /^native\/tests\/.+\.py$/;

/** native/.clang-format uses options clang-format has had since this version. */
const CLANG_FORMAT_MAJOR = 20;

/** Where the LLVM installer for Windows puts clang-format. */
const WINDOWS_CLANG_FORMAT = 'C:/Program Files/LLVM/bin/clang-format.exe';

function main() {
  const staged = process.argv.includes('--staged');
  process.chdir(git(['rev-parse', '--show-toplevel']).trim());

  const listing = staged
    ? git(['diff', '--cached', '--name-only', '-z', '--diff-filter=ACMR'])
    : git(['ls-files', '-z', '--cached', '--others', '--exclude-standard']);
  const paths = listing.split('\0').filter(Boolean);
  const cSources = paths.filter((path) => C_SOURCE.test(path));
  const pythonSources = paths.filter((path) => PYTHON_SOURCE.test(path));

  let failed = false;

  // The C sources: clang-format reports every line it would change, and fails on the first one.
  if (cSources.length > 0) {
    const clean = checkWith('clang-format', clangFormat(), staged, (tool) => run(tool, ['--dry-run', '--Werror', ...cSources]));
    failed ||= !clean;
  }

  // The Python tests: the layout first, then the rules ruff.toml selects.
  if (pythonSources.length > 0) {
    const clean = checkWith(
      'ruff',
      ruff(),
      staged,
      (tool) => run(tool, ['format', '--check', ...pythonSources]) && run(tool, ['check', ...pythonSources]),
    );
    failed ||= !clean;
  }

  if (failed) {
    process.exitCode = 1;
    return;
  }

  process.stdout.write(`check-format: ${cSources.length} C and ${pythonSources.length} Python file(s) clean\n`);
}

/**
 * Runs a check with a tool, or says the tool is missing. Under --staged a missing tool is skipped.
 *
 * @param {string} name
 * @param {string[] | undefined} tool
 * @param {boolean} staged
 * @param {(tool: string[]) => boolean} check
 * @returns {boolean} whether the files passed, or were skipped
 */
function checkWith(name, tool, staged, check) {
  if (tool === undefined) {
    process.stderr.write(`check-format: ${name} not found${staged ? ', skipped (CI checks it)' : ''}\n`);
    return staged;
  }

  return check(tool);
}

/**
 * clang-format of a version that understands native/.clang-format, as a command.
 *
 * @returns {string[] | undefined}
 */
function clangFormat() {
  const candidates = [process.env.CLANG_FORMAT, 'clang-format', fs.existsSync(WINDOWS_CLANG_FORMAT) ? WINDOWS_CLANG_FORMAT : undefined];

  for (const candidate of candidates) {
    if (candidate === undefined || candidate === '') {
      continue;
    }

    const version = /version (\d+)\./.exec(versionOf([candidate]) ?? '');
    if (version !== null && Number(version[1]) >= CLANG_FORMAT_MAJOR) {
      return [candidate];
    }
  }

  return undefined;
}

/**
 * ruff, as a command: the executable itself, or the module of a Python that has it.
 *
 * @returns {string[] | undefined}
 */
function ruff() {
  /** @type {string[][]} */
  const candidates = [['ruff'], ['py', '-3', '-m', 'ruff'], ['python3', '-m', 'ruff'], ['python', '-m', 'ruff']];
  if (process.env.RUFF !== undefined && process.env.RUFF !== '') {
    candidates.unshift([process.env.RUFF]);
  }

  return candidates.find((candidate) => versionOf(candidate) !== undefined);
}

/**
 * What a command prints for --version, or nothing when it cannot be run.
 *
 * @param {string[]} command
 * @returns {string | undefined}
 */
function versionOf(command) {
  const result = spawnSync(command[0], [...command.slice(1), '--version'], { encoding: 'utf8' });
  return result.status === 0 ? result.stdout : undefined;
}

/**
 * Runs a command with its output on the terminal and says whether it succeeded.
 *
 * @param {string[]} command
 * @param {string[]} args
 * @returns {boolean}
 */
function run(command, args) {
  return spawnSync(command[0], [...command.slice(1), ...args], { stdio: 'inherit' }).status === 0;
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

main();
