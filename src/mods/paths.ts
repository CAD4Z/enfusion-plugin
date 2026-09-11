/**
 * The path arithmetic the domain does.
 *
 * Paths are `/` separated throughout — that is what `Uri.path` hands over on every platform — and
 * nothing here goes near a disk: these are string operations on paths a search already found.
 *
 * The exception is the second half of this file. A command line, a junction, a folder a builder
 * writes into: none of those take a `Uri.path`, and the arithmetic for them is its own.
 */

/** True for the folder itself as well, which is what makes a mod root usable as a prefix root. */
export function isWithin(folder: string, root: string): boolean {
  return folder === root || folder.startsWith(`${root}/`);
}

/** The folder the path sits in. */
export function folderOf(path: string): string {
  return path.slice(0, path.lastIndexOf('/'));
}

/** The last segment: a file's name, or a folder's own name. */
export function nameOf(path: string): string {
  return path.slice(path.lastIndexOf('/') + 1);
}

/**
 * A path reduced to what a comparison should look at. Windows tells none of these apart — the
 * case, the separator, a doubled or trailing one, a `.` segment, a `..` the path walks back
 * through, the leading slash a `Uri.path` carries in front of a drive letter — so neither does
 * anything here that compares two paths. A root never walks above itself, which is what keeps
 * `C:\..` and a UNC share whole.
 */
export function samePath(path: string): string {
  const slashed = path.replace(/\\/g, '/').replace(/^\/(?=[A-Za-z]:)/, '');
  const share = slashed.startsWith('//');
  const rooted = !share && slashed.startsWith('/');
  const parts = slashed.split('/').filter((part) => part !== '' && part !== '.');
  // A root is never walked above: the drive letter, or the server and share naming a UNC root.
  const root = share ? `//${parts.splice(0, 2).join('/')}`
    : rooted ? '/'
    : /^[A-Za-z]:$/.test(parts[0] ?? '') ? parts.shift() ?? ''
    : '';
  const segments: string[] = [];
  for (const part of parts) {
    if (part !== '..') {
      segments.push(part);
    } else if (segments.length > 0 && segments[segments.length - 1] !== '..') {
      segments.pop();
    } else if (root === '') {
      segments.push(part);
    }
  }

  const below = segments.join('/');
  const joined = below === '' ? root
    : root === '' ? below
    : root === '/' ? `/${below}`
    : `${root}/${below}`;
  return joined.toLowerCase();
}

/** Either separator, because a path typed into a manifest is typed whichever way. */
const SEPARATOR = /[\\/]/;

const TRAILING = /[\\/]+$/;

const LEADING = /^[\\/]+/;

/**
 * Joins the parts with `\`, dropping the empty ones and never doubling a separator. Only the first
 * part keeps a separator in front of it, which is what leaves `\\server\share` a UNC path while a
 * part below it — a path counted from the root of a drive, say — joins on cleanly.
 */
export function windowsPath(...parts: readonly string[]): string {
  return parts
    .map((part, index) => (index === 0 ? part : part.replace(LEADING, '')))
    .map((part) => part.replace(TRAILING, ''))
    .filter((part) => part !== '')
    .join('\\');
}

/** The last segment of a Windows path: a file's name, or a folder's own name. */
export function windowsName(path: string): string {
  return path.replace(TRAILING, '').split(SEPARATOR).at(-1) ?? '';
}

/** The folder a Windows path sits in; empty when there is nothing above it. */
export function windowsFolder(path: string): string {
  const trimmed = path.replace(TRAILING, '');
  const at = Math.max(trimmed.lastIndexOf('\\'), trimmed.lastIndexOf('/'));

  return at === -1 ? '' : trimmed.slice(0, at);
}

/**
 * A path out of a manifest, taken the way the file holding it means it. A `.enf` is text under
 * git, so a path worth writing in one is relative to the file itself — an absolute one is only
 * ever right on the machine it was typed on. Anything already rooted is left as it was typed.
 */
export function resolveWindows(base: string, path: string): string {
  const value = path.trim().replace(/\//g, '\\');
  if (value === '') {
    return '';
  }

  // `C:\...`, `\\server\share`, and `\rooted`, which Windows counts from the current drive.
  return /^([A-Za-z]:|\\)/.test(value) ? value : windowsPath(base, value);
}
