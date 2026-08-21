/**
 * The path arithmetic the domain does.
 *
 * Paths are `/` separated throughout — that is what `Uri.path` hands over on every platform — and
 * nothing here goes near a disk: these are string operations on paths a search already found.
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
 * case, the separator, a trailing one, the leading slash a `Uri.path` carries in front of a drive
 * letter — so neither does anything here that compares two paths.
 */
export function samePath(path: string): string {
  return path
    .replace(/\\/g, '/')
    .replace(/^\/(?=[A-Za-z]:)/, '')
    .replace(/\/+$/, '')
    .toLowerCase();
}
