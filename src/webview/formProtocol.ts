/**
 * What crosses between the `.enf` editor and the form it puts over the file.
 *
 * The form is handed the file already read — the fields, the launch block, what is wrong with it —
 * and hands back one move at a time. It never sends text: where in the file a move lands and what
 * that comes to as an edit is the domain's arithmetic, done on the extension side, so a form that
 * has misunderstood the file cannot write anything but a field it was shown.
 */

import type { Launch, ManifestProblem, ModManifest } from '../mods/enf';
import type { FormEdit, ManifestKind } from '../mods/form';

/** Sent to the form whenever the file it is over can have changed, the form's own edits included. */
export interface ManifestMessage {
  readonly type: 'manifest';
  readonly kind: ManifestKind;
  /** The file's name, which is what the form is headed by. */
  readonly file: string;
  /** What the mod says about itself; a `workspace.enf` says nothing about any one mod. */
  readonly mod: ModManifest | undefined;
  readonly launch: Launch;
  readonly problems: readonly ManifestProblem[];
  /** Why the form is showing the file without writing into it; undefined when it writes. */
  readonly refusal: string | undefined;
  /**
   * Where the `workspace.enf` that owns this mod's launch block sits, relative to the open folder.
   * Undefined where this file owns its own launch — which is every `workspace.enf`, and every
   * `mod.enf` with none above it.
   */
  readonly ownedBy: string | undefined;
}

/** Sent by the form. `ready` also comes after the editor is hidden and shown again. */
export type FormRequest =
  | { readonly type: 'ready' }
  /** One move of the form, which the extension turns into one edit of the document. */
  | { readonly type: 'edit'; readonly edit: FormEdit }
  /** Opens the same file as text, at the place a problem is when the form names one. */
  | { readonly type: 'text'; readonly line?: number; readonly column?: number };
