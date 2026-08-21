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

/** Sent to the panel whenever the mods, or the machine they are built on, can have changed. */
export interface ModsMessage {
  readonly type: 'mods';
  readonly environment: EnvironmentView;
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
  | { readonly type: 'settings'; readonly id: string };
