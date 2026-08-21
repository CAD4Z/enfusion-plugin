/**
 * Reading what a builder wrote about a build.
 *
 * A failed build says where it failed, and the point of reading it is that the developer lands on
 * the line rather than on a wall of text. Two shapes carry a place, and both were taken off real
 * logs of the real tools rather than guessed at:
 *
 *     \EnfusionProbe\config.cpp Rapify:circa Line 6 Expected Semicolon OR bad array syntax
 *     [ERROR]: !> File P:\EnfusionProbe\config.cpp, line 2: Undefined base class 'NoSuchBaseAtAll'
 *
 * The first is pboProject's own, with the file named from the root of the work drive and the line
 * given as approximate — `circa`, and it means it. The second is the engine's config parser, which
 * is what AddonBuilder passes through and what every Bohemia tool has printed for twenty years.
 *
 * A file is named as the builder saw it, which is somewhere on the work drive, and the work drive
 * is junctions. So the last thing here puts a reported path back through the links onto the file
 * in the workspace — the one the developer has open, rather than a second way to reach the same
 * bytes that opens a second tab.
 *
 * Nothing here decides anything about severity: a build either produced its pbo or it did not, and
 * that is the caller's fact to apply.
 */

import { samePath, windowsPath } from './paths';
import type { Link } from './workDrive';

/** Something a builder said about a place in a file. */
export interface PackingProblem {
  /** The file as the builder named it: on the work drive, or counted from the root of it. */
  readonly file: string;
  /** 1-based, and approximate where the builder said it was. */
  readonly line: number;
  readonly message: string;
}

/**
 * pboProject: the file, then `Rapify:circa Line <n>`, then what is wrong. It prints the same
 * complaint again on the next line without the file, and that second copy is left alone — it has
 * nowhere to send anybody.
 */
const RAPIFY = /^(.+?) Rapify:circa Line (\d+)\s*(.*)$/;

/**
 * The config parser: `File <path>, line <n>: <message>`. Whatever the log put in front of it —
 * a timestamp, a level, AddonBuilder's `!>` — is skipped rather than matched, because the parser's
 * own words are the only part of the line that means anything.
 */
const CONFIG = /File (.+?), line (\d+):\s*(.*)$/;

/** Everything the builder said about a place, in the order it said it and each said once. */
export function problemsOf(log: string): PackingProblem[] {
  const found: PackingProblem[] = [];
  const seen = new Set<string>();

  for (const line of log.split('\n')) {
    const problem = problemOf(line.trim());

    if (problem !== undefined && !seen.has(keyOf(problem))) {
      seen.add(keyOf(problem));
      found.push(problem);
    }
  }

  return found;
}

/**
 * The file in the workspace a builder's path points at.
 *
 * A path with no drive on it is counted from the root of the work drive, which is where a prefix
 * is counted from. From there it is whichever mod's link it falls under, swapped for what that
 * link points at. A path under no link at all — a vanilla file, a mod nobody linked — is handed
 * back as it was: it still opens, through the drive itself.
 */
export function fileOf(reported: string, letter: string, links: readonly Link[]): string {
  const path = /^[A-Za-z]:/.test(reported) ? reported : windowsPath(letter, reported);
  const wanted = samePath(path);

  for (const link of links) {
    const root = samePath(link.path);

    if (wanted === root) {
      return link.target;
    }

    if (wanted.startsWith(`${root}/`)) {
      return windowsPath(link.target, path.slice(root.length + 1));
    }
  }

  return path;
}

function problemOf(line: string): PackingProblem | undefined {
  const rapify = RAPIFY.exec(line);
  if (rapify) {
    return problemFrom(rapify);
  }

  const config = CONFIG.exec(line);
  return config ? problemFrom(config) : undefined;
}

function problemFrom(found: RegExpExecArray): PackingProblem | undefined {
  const file = (found[1] ?? '').trim();
  const line = Number(found[2]);
  const message = (found[3] ?? '').trim();

  return file === '' || message === '' ? undefined : { file, line, message };
}

function keyOf(problem: PackingProblem): string {
  return `${samePath(problem.file)}:${problem.line}:${problem.message}`;
}
