/**
 * What crosses between the extension and the panel.
 *
 * The panel is handed mods that are already in the shape it shows them: it does no path
 * arithmetic and knows nothing about `Uri`, so a path it sends back is only ever one the
 * extension gave it in the first place.
 */

import type { Problem } from '../mods/model';

/** Sent to the panel whenever the mods can have changed. */
export interface ModsMessage {
  readonly type: 'mods';
  readonly mods: readonly ModView[];
}

export interface ModView {
  readonly name: string;
  /** Where the mod sits, relative to the open folder. */
  readonly location: string;
  /** False for a mod found by its `config.cpp` alone, which has no `mod.enf` yet. */
  readonly configured: boolean;
  readonly addons: readonly AddonView[];
  readonly problems: readonly Problem[];
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
  | { readonly type: 'open'; readonly path: string };
