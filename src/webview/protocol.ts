/**
 * What crosses between the extension and the panel.
 *
 * The panel is handed mods that are already in the shape it shows them: it does no path
 * arithmetic and knows nothing about `Uri`, so a path it sends back is only ever one the
 * extension gave it in the first place. The same goes for the words: what a button does, and why
 * it would refuse, is written where the letter of the work drive and the name of the mod are
 * known, and the panel only renders it.
 */

import type { ManifestProblem } from '../mods/enf';
import type { Problem } from '../mods/model';
import type { LinkState, WorkDriveAction } from '../mods/workDrive';

/** Sent to the panel whenever the mods, or the machine they are built on, can have changed. */
export interface ModsMessage {
  readonly type: 'mods';
  readonly tools: ToolsView;
  /** The `workspace.enf` files of the open folders; usually none, and at most one that matters. */
  readonly workspaces: readonly ManifestFileView[];
  readonly mods: readonly ModView[];
}

/** One button: what it does, and why it would not do it as things stand. */
export interface ActionView {
  /** What the tooltip says, where the button would work. */
  readonly title: string;
  /** Why it would refuse instead, which is what a disabled button says without being pressed. */
  readonly refusal: string | undefined;
}

/** The row of buttons above everything: start the game, build it, and put the work drive up. */
export interface ToolsView {
  readonly start: ActionView;
  /** The second client, which joins the launch that is already up rather than starting one. */
  readonly secondClient: ActionView;
  readonly build: ActionView;
  readonly workDrive: readonly WorkDriveActionView[];
}

export interface WorkDriveActionView extends ActionView {
  readonly action: WorkDriveAction;
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
  /** The mod's one name: what it is called, linked and loaded as. */
  readonly name: string;
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
  /** Where it sits, relative to the open folder, which is what its row is titled by. */
  readonly location: string;
  /** The mods whose launch block this file owns — the ones with no nearer file above them. */
  readonly owns: readonly string[];
  readonly problems: readonly ManifestProblem[];
}

export interface AddonView {
  /** Folder name, which is what the addon is known and asked for by. */
  readonly name: string;
  readonly main: boolean;
  /** The `config.cpp` to open, named the way the extension knows it. */
  readonly config: string;
  readonly patches: readonly string[];
}

/** Sent by the panel. `ready` also comes after the panel is hidden and shown again. */
export type PanelRequest =
  | { readonly type: 'ready' }
  | { readonly type: 'refresh' }
  /** Restarts the extension host, which is what a page older or newer than it needs. */
  | { readonly type: 'reload' }
  /** Opens the file, at the place the problem is when the panel names one. */
  | {
      readonly type: 'open';
      readonly path: string;
      readonly line?: number;
      readonly column?: number;
    }
  /** Puts the game up: the target chosen on the status bar, the way F5 does. */
  | { readonly type: 'launch' }
  /** Adds a second client to the launch that is up, with the Steam the machine settings name. */
  | { readonly type: 'launchSecondClient' }
  /** Runs the work drive command of that action, which the palette runs the same way. */
  | { readonly type: 'workDrive'; readonly action: WorkDriveAction }
  /** Builds one addon, named the way the panel was given it. */
  | { readonly type: 'build'; readonly mod: string; readonly addon: string }
  /** Builds every addon of the workspace, in the order the graph puts them. */
  | { readonly type: 'buildAll' }
  /** Makes a mod, which is what an empty workspace has to offer. */
  | { readonly type: 'init' }
  /** Writes the `mod.enf` an unconfigured mod has not got, named the way the panel was given it. */
  | { readonly type: 'adopt'; readonly mod: string }
  /** Adds an addon to the mod, named the way the panel was given it. */
  | { readonly type: 'addon'; readonly mod: string };
