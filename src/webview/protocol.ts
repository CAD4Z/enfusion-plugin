/**
 * What crosses between the extension and the panel.
 *
 * The panel is handed mods that are already in the shape it shows them: it does no path
 * arithmetic and knows nothing about `Uri`, so a path it sends back is only ever one the
 * extension gave it in the first place.
 */

import type { ManifestProblem } from '../mods/enf';
import type { EnvironmentEntry } from '../mods/machine';
import type { Problem } from '../mods/model';
import type { LinkState, WorkDriveAction, WorkDriveState } from '../mods/workDrive';

/** Sent to the panel whenever the mods, or the machine they are built on, can have changed. */
export interface ModsMessage {
  readonly type: 'mods';
  readonly environment: EnvironmentView;
  readonly workDrive: WorkDriveView;
  /** The `workspace.enf` files of the open folders; usually none, and at most one that matters. */
  readonly workspaces: readonly ManifestFileView[];
  readonly mods: readonly ModView[];
}

/** What of the machine resolved and what did not, which is a refusal seen before the first build. */
export interface EnvironmentView {
  readonly entries: readonly EnvironmentEntry[];
  /** How many of them are a gap rather than a choice, which is what the section is headed by. */
  readonly wanting: number;
}

/** Where the work drive is, against where it should be, and what can be done about it. */
export interface WorkDriveView {
  readonly letter: string;
  /** The folder the settings mount it from. */
  readonly source: string;
  /** The folder it is mounted from now; empty when the letter is free. */
  readonly at: string;
  readonly state: WorkDriveState;
  /** The setting that names the folder, for the row that opens it. */
  readonly setting: string;
  /** Why the drive is not as it should be, in the words the refused button would use. */
  readonly warning: string | undefined;
  readonly actions: readonly WorkDriveActionView[];
  /** How many mods are not on the drive where they should be, which heads the section. */
  readonly unlinked: number;
}

export interface WorkDriveActionView {
  readonly action: WorkDriveAction;
  /** Why it would refuse as things stand; undefined where it would work. */
  readonly refusal: string | undefined;
}

/** One mod's place on the work drive, which is why a build would find its sources or would not. */
export interface LinkView {
  readonly state: LinkState;
  /** `P:\<Name>`: what the mod is linked as. */
  readonly path: string;
  /** Where that points now; empty when nothing is there. */
  readonly at: string;
}

export interface ModView {
  /** The prefix root's name: what the mod is linked and loaded as. */
  readonly name: string;
  /** Where the mod sits, relative to the open folder. */
  readonly location: string;
  /** The name `mod.enf` gives the mod, where it gives one. */
  readonly title: string | undefined;
  readonly description: string | undefined;
  /** The `mod.enf` to open; undefined for a mod found by its `config.cpp` alone. */
  readonly manifest: string | undefined;
  /** What is wrong with that `mod.enf`, and where. */
  readonly manifestProblems: readonly ManifestProblem[];
  /** Where it sits on the work drive; undefined for a mod with no prefix root to link. */
  readonly link: LinkView | undefined;
  readonly addons: readonly AddonView[];
  readonly problems: readonly Problem[];
}

export interface ManifestFileView {
  /** The file to open, named the way the extension knows it. */
  readonly path: string;
  /** Where it sits, relative to the open folder. */
  readonly location: string;
  /** The mods whose launch block this file owns — the ones with no nearer file above them. */
  readonly owns: readonly string[];
  readonly problems: readonly ManifestProblem[];
}

export interface AddonView {
  /** Folder name, which is also the name of the pbo it packs into. */
  readonly name: string;
  readonly main: boolean;
  /** The `config.cpp` to open, named the way the extension knows it. */
  readonly config: string;
  readonly patches: readonly string[];
  /** Required addons no addon of this workspace declares. */
  readonly unresolved: readonly string[];
}

/** Sent by the panel. `ready` also comes after the panel is hidden and shown again. */
export type PanelRequest =
  | { readonly type: 'ready' }
  | { readonly type: 'refresh' }
  /** Opens the file, at the place the problem is when the panel names one. */
  | {
      readonly type: 'open';
      readonly path: string;
      readonly line?: number;
      readonly column?: number;
    }
  /** Opens the settings on the one that is missing. */
  | { readonly type: 'settings'; readonly id: string }
  /** Runs the work drive command of that action, which the palette runs the same way. */
  | { readonly type: 'workDrive'; readonly action: WorkDriveAction }
  /** Builds one addon, named the way the panel was given it. */
  | { readonly type: 'build'; readonly mod: string; readonly addon: string }
  /** Makes a mod, which is what an empty workspace has to offer. */
  | { readonly type: 'init' }
  /** Adds an addon to the mod, named the way the panel was given it. */
  | { readonly type: 'addon'; readonly mod: string };
