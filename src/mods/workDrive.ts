/**
 * The work drive: the letter every resource path in the game is counted from, and the links that
 * put the mods of the workspace on it.
 *
 * Two facts make the whole of it. Where the letter points now — what `subst` answered — against
 * where the settings say it should point: a drive mounted from a folder nobody configured is the
 * case this exists to catch, because everything built on it would be built off the wrong sources
 * and nothing would say so. And what sits at `P:\<Name>` for each mod: a junction to the prefix
 * root, a junction to somewhere else, a real folder, or nothing at all.
 *
 * Nothing here goes near a disk. Where the drive is and what is at each link are handed in as
 * plain strings; what any of it means, and what a button would refuse to do, is decided here.
 */

import { samePath } from './paths';

/** The letter the work drive goes up under unless the settings name another. */
export const DEFAULT_LETTER = 'P:';

/**
 * The letter as the tools take it, out of however it was typed: `P`, `p:`, `P:\` all mean the
 * same drive. Anything that is not a letter at all — a folder pasted into the wrong setting — is
 * the default, and the setting's own pattern is what tells the developer about it.
 */
export function driveLetterOf(value: string): string {
  const letter = /^([A-Za-z]):?[\\/]?$/.exec(value.trim())?.[1];

  return letter === undefined ? DEFAULT_LETTER : `${letter.toUpperCase()}:`;
}

/** Where the letter points, against where it was set to point. */
export type WorkDriveState =
  /** No folder is set, so there is nothing to mount and nothing to check against. */
  | 'unset'
  | 'unmounted'
  | 'mounted'
  /** Mounted, but from a folder other than the one that was set. */
  | 'elsewhere';

export interface WorkDrive {
  /** `P:`, or whatever else the settings say. */
  readonly letter: string;
  /** The folder the settings mount it from; empty when nobody set one. */
  readonly source: string;
  /** The folder it is mounted from now; empty when the letter is free. */
  readonly at: string;
  readonly state: WorkDriveState;
}

export function workDriveOf(letter: string, source: string, at: string): WorkDrive {
  return { letter, source, at, state: stateOf(source, at) };
}

/** The three things a developer does to the work drive, and the panel has a button for. */
export type WorkDriveAction = 'mount' | 'unmount' | 'link';

/** In the order the panel puts the buttons in. */
export const WORK_DRIVE_ACTIONS: readonly WorkDriveAction[] = ['mount', 'unmount', 'link'];

/**
 * Why the action cannot be done as things stand, or undefined when it can. Said as a sentence,
 * because the same words are the tooltip on the disabled button and the message the command shows
 * when it is asked for from the palette instead.
 */
export function refusalOf(drive: WorkDrive, action: WorkDriveAction): string | undefined {
  // Freeing a letter is worth doing whatever the settings say the letter was meant to hold.
  if (action === 'unmount') {
    return drive.at === '' ? `${drive.letter} is not mounted.` : undefined;
  }

  switch (drive.state) {
    case 'unset':
      return 'No folder is set to mount the work drive from.';
    case 'unmounted':
      return action === 'mount' ? undefined : `${drive.letter} is not mounted.`;
    case 'mounted':
      return action === 'mount'
        ? `${drive.letter} is already mounted from ${drive.source}.`
        : undefined;
    case 'elsewhere':
      return `${drive.letter} is mounted from ${drive.at}, not from ${drive.source}.`;
  }
}

/** A mod as the work drive sees it: the name it goes up under, and the folder that goes there. */
export interface Prefix {
  /**
   * The prefix root as the model has it, which is what a link is looked back up by. The mod root
   * would not do: two mods with no `mod.enf` sitting side by side share one, and each would find
   * the other's link.
   */
  readonly prefixRoot: string;
  /** The prefix root's name: what the mod is linked and loaded as. */
  readonly name: string;
  /** The prefix root itself, the way Windows takes it. */
  readonly target: string;
}

/** What is at `P:\<Name>`, as the disk answered. */
export type LinkFact =
  | { readonly kind: 'none' }
  | { readonly kind: 'link'; readonly target: string }
  /** A real folder, a file, a link nothing could be read out of: something not ours to remove. */
  | { readonly kind: 'occupied' };

export type LinkState =
  | 'linked'
  | 'unlinked'
  /** A link, but to some other folder — the build would read sources nobody asked for. */
  | 'elsewhere'
  | 'occupied'
  /** The drive is down, so `P:\<Name>` is not a question with an answer. */
  | 'unavailable';

/** One mod's place on the work drive: where it should be linked, and what is there instead. */
export interface Link {
  readonly prefixRoot: string;
  readonly name: string;
  /** `P:\<Name>`. */
  readonly path: string;
  readonly target: string;
  /** Where the link points now; empty when nothing is there. */
  readonly at: string;
  readonly state: LinkState;
}

/**
 * Where a mod goes on the drive: the root of it and never below, because the prefix of a pbo is
 * the path of its addon on the work drive, and a mod one folder deeper prefixes everything wrong.
 */
export function linkPathOf(letter: string, name: string): string {
  return `${letter}\\${name}`;
}

/**
 * Every mod's place on the drive, from where it should be and what the disk said is there. A drive
 * mounted from the wrong folder is not a reason to say nothing about the links: where `P:\<Name>`
 * points is a fact of its own, and the drive being wrong is reported as the drive being wrong.
 */
export function linksOf(
  prefixes: readonly Prefix[],
  drive: WorkDrive,
  facts: ReadonlyMap<string, LinkFact>,
): Link[] {
  const down = drive.at === '';

  return prefixes.map((prefix) => {
    const path = linkPathOf(drive.letter, prefix.name);
    const fact: LinkFact = (down ? undefined : facts.get(path)) ?? { kind: 'none' };

    return {
      prefixRoot: prefix.prefixRoot,
      name: prefix.name,
      path,
      target: prefix.target,
      at: fact.kind === 'link' ? fact.target : '',
      state: down ? 'unavailable' : linkStateOf(fact, prefix.target),
    };
  });
}

/** A mod that is not on the drive where it should be, which is what the command is for. */
export function isUnlinked(link: Link): boolean {
  return link.state !== 'linked' && link.state !== 'unavailable';
}

/**
 * The links a run of the command would make. A junction already pointing where it should is left
 * alone rather than remade, and one pointing elsewhere is repointed — that is the difference
 * between a second run being free and a second run being an argument. What is occupied by
 * something that is not a link stays out of it: a real folder on the drive is somebody's data.
 *
 * A path is claimed once. Two mods of the same name want the one `P:\<Name>` and only one of them
 * can have it; the first keeps it every run, and the second is left showing that it points
 * elsewhere — which says there is a clash — rather than the two of them taking turns overwriting
 * each other, or the second failing and stopping the run with the rest of the mods unlinked.
 */
export function linksToMake(links: readonly Link[]): Link[] {
  const claimed = new Set<string>();

  return links.filter((link) => {
    const path = samePath(link.path);
    if (claimed.has(path)) {
      return false;
    }
    claimed.add(path);

    return link.state === 'unlinked' || link.state === 'elsewhere';
  });
}

function stateOf(source: string, at: string): WorkDriveState {
  if (source === '') {
    return 'unset';
  }

  if (at === '') {
    return 'unmounted';
  }

  return samePath(at) === samePath(source) ? 'mounted' : 'elsewhere';
}

function linkStateOf(fact: LinkFact, target: string): LinkState {
  switch (fact.kind) {
    case 'none':
      return 'unlinked';
    case 'occupied':
      return 'occupied';
    case 'link':
      return samePath(fact.target) === samePath(target) ? 'linked' : 'elsewhere';
  }
}
