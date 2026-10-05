/**
 * What crosses between the extension and the panel.
 *
 * The panel is handed mods that are already in the shape it shows them: it does no path
 * arithmetic and knows nothing about `Uri`, so a path it sends back is only ever one the
 * extension gave it in the first place. The same goes for the words: what a button does, and why
 * it would refuse, is written where the letter of the work drive and the name of the mod are
 * known, and the panel only renders it.
 */

import type { ManifestProblem } from '../../mods/enf';
import type { Problem } from '../../mods/model';
import type { LinkState, WorkDriveAction } from '../../mods/workDrive';

/** Sent to the panel whenever the mods, or the machine they are built on, can have changed. */
export interface ModsMessage {
  readonly type: 'mods';
  readonly tools: ToolsView;
  /** The `workspace.enf` files of the open folders; usually none, and at most one that matters. */
  readonly workspaces: readonly ManifestFileView[];
  readonly mods: readonly ModView[];
  /**
   * What a panel with no mod in it offers instead of a list: present exactly when `mods` is empty.
   * The buttons over the list act on mods, so a panel without one shows this and not them.
   */
  readonly empty: EmptyView | undefined;
  /**
   * Why the buttons that run a program are off in a folder the editor does not trust, said over
   * them rather than only in their tooltips; undefined in a trusted one.
   */
  readonly restricted: string | undefined;
}

/** The middle of an empty panel: why it is empty, and the ways out, the first one foremost. */
export interface EmptyView {
  readonly lines: readonly string[];
  readonly actions: readonly EmptyAction[];
}

/** What an empty panel offers: a mod, a workspace, or a folder to make either in. */
export type EmptyAction = 'init' | 'initWorkspace' | 'openFolder';

/** One button: what it does, and why it would not do it as things stand. */
export interface ActionView {
  /** What the tooltip says, where the button would work. */
  readonly title: string;
  /** Why it would refuse instead, which is what a disabled button says without being pressed. */
  readonly refusal: string | undefined;
}

/**
 * One thing a list offers: what it is called, and nothing else.
 *
 * A name and no second line under it. A target's name and the word `Debug` are what a developer
 * reads to know which launch this is, and what a target runs or what a build means is on the
 * button underneath, where it is read once rather than four times over.
 */
export interface ChoiceView {
  /** What is sent back when it is picked, which is one the extension gave in the first place. */
  readonly id: string;
  readonly label: string;
}

/** A closed set of answers, and the one that stands. */
export interface PickerView {
  readonly options: readonly ChoiceView[];
  /** The chosen one's id; empty where a workspace of several targets has not been asked yet. */
  readonly chosen: string;
  /** What the whole list is for, on hover. */
  readonly title: string;
  /** Why there is nothing to pick, which is what a list of nothing says instead of being blank. */
  readonly refusal: string | undefined;
}

/**
 * The row above everything: what the next launch is, and the buttons that act on the whole
 * workspace. The two lists come first because they are what the buttons under them do — a Start
 * that says which target and which build it is about is a Start that gets pressed with confidence.
 */
export interface ToolsView {
  /** Which target the next launch puts up: the maps, out of the `.enf`. */
  readonly target: PickerView;
  /** And which build it puts up: the diag one, or the pair a player runs. */
  readonly gameBuild: PickerView;
  readonly start: ActionView;
  /** The second client, which joins the launch that is already up rather than starting one. */
  readonly secondClient: ActionView;
  readonly build: ActionView;
  readonly workDrive: readonly WorkDriveActionView[];
  readonly workbench: ActionView;
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
  /** Why an invalid name has no path; undefined for every other state. */
  readonly problem: string | undefined;
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
  /** Puts the game up: the target and build the row above says, the way F5 does. */
  | { readonly type: 'launch' }
  /** Which target the next launch puts up, named the way the panel was offered it. */
  | { readonly type: 'selectTarget'; readonly id: string }
  /** And which build, likewise. */
  | { readonly type: 'selectGameBuild'; readonly build: string }
  /** Adds a second client to the launch that is up, with the Steam the machine settings name. */
  | { readonly type: 'launchSecondClient' }
  /** Runs the work drive command of that action, which the palette runs the same way. */
  | { readonly type: 'workDrive'; readonly action: WorkDriveAction }
  /** Opens the selected target mod's `.gproj` in DayZ Workbench. */
  | { readonly type: 'workbench' }
  /** Builds one addon, named the way the panel was given it. */
  | { readonly type: 'build'; readonly mod: string; readonly addon: string }
  /** Builds every addon of the workspace, in the order the graph puts them. */
  | { readonly type: 'buildAll' }
  /** Makes a mod, which is what an empty workspace has to offer. */
  | { readonly type: 'init' }
  /** Makes a workspace, which is the other thing a folder with no mod in it can become. */
  | { readonly type: 'initWorkspace' }
  /** Opens a folder, which is what a window with none has to do before anything else. */
  | { readonly type: 'openFolder' }
  /** Opens the editor's own question of whether this folder is trusted. */
  | { readonly type: 'trust' }
  /** Writes the `mod.enf` an unconfigured mod has not got, named the way the panel was given it. */
  | { readonly type: 'adopt'; readonly mod: string }
  /** Adds an addon to the mod, named the way the panel was given it. */
  | { readonly type: 'addon'; readonly mod: string };
