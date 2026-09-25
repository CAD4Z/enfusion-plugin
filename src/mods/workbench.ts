/**
 * Choosing the Workbench project that belongs to the mod a launch target names.
 *
 * A project is a file discovered under the mod root. `dayz.gproj` is the DayZ convention and wins
 * where a mod keeps helper projects beside it; otherwise the shallowest, alphabetically first
 * project is deterministic and visible in the button tooltip before anything is started.
 */

import { nameOf, samePath } from './paths';

/** The `.gproj` under this mod root that Workbench should open. */
export function projectOf(modRoot: string, projects: readonly string[]): string | undefined {
  const root = samePath(modRoot);

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
  return nameOf(samePath(path)) === 'dayz.gproj';
}
