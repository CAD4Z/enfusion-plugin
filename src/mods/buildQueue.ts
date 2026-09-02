/**
 * The line a press on a build button goes into, and what the presses after it do to the ones
 * already standing in it.
 *
 * A build takes as long as it takes, and a developer pressing the button again is not asking
 * whether the first one is running: they have changed something since, and they want that packed.
 * So a second press is queued rather than refused. Nothing is ever turned away, and nothing runs
 * beside the build that is already running — two builds of one workspace do not share the work,
 * they race for the same pbo, one taking the file off while the other writes it. The queue is
 * a line, never a fan.
 *
 * The line folds, and the fold is what keeps the line from being a pile. A press for something
 * already waiting adds nothing: what it is waiting for has not started, and starting it twice
 * packs the same sources into the same pbo twice. That is the whole of the answer to the held key
 * that once left five hundred builders on a machine — the fold is at the front of the line rather
 * than at the process, so the presses cost nothing however many of them there are.
 *
 * What is *running* is never folded into. It read its sources when it started; a press that came
 * after it is asking for the sources as they are now, which is exactly the case of a developer
 * fixing something while the build that will fail on it is still going.
 */

/** What one press asks for: one addon of one mod, or every addon of the workspace. */
export type BuildRequest =
  | { readonly kind: 'addon'; readonly mod: string; readonly addon: string }
  | { readonly kind: 'all' };

/** The line after a press, and whether that press put anything new in it. */
export interface Queued {
  readonly waiting: readonly BuildRequest[];
  /** False where the press was folded into what was already waiting. */
  readonly added: boolean;
}

/**
 * The line this press leaves behind it.
 *
 * Building the lot swallows the addons waiting on their own — it packs every one of them, in the
 * order the graph puts them, so keeping their places would only pack them twice. Which leaves the
 * line in one of two shapes and no third: a run of addons, or the single request for all of them.
 */
export function queued(waiting: readonly BuildRequest[], request: BuildRequest): Queued {
  if (request.kind === 'all') {
    return { waiting: [request], added: !waiting.some((it) => it.kind === 'all') };
  }

  // Nothing to add behind a build of everything: it will pack this addon along with the rest.
  const coming = waiting.some((it) => it.kind === 'all' || isSameRequest(it, request));

  return coming ? { waiting, added: false } : { waiting: [...waiting, request], added: true };
}

/**
 * What the notification is titled by and the log names it as. `<Mod>\<Addon>` is what the panel
 * called it rather than what the plan will call it — a single-addon mod is packed under its own
 * name, and that name is not known until the workspace has been read.
 */
export function nameOf(request: BuildRequest): string {
  if (request.kind === 'all') {
    return 'every addon';
  }

  return request.mod === request.addon ? request.mod : `${request.mod}\\${request.addon}`;
}

/**
 * Whether two presses ask for the very same thing. Which is what one folding into another is
 * decided by, both in the line and in the moment after a build has started — see the settle window
 * in `src/view/build.ts`.
 */
export function isSameRequest(one: BuildRequest, other: BuildRequest): boolean {
  if (one.kind === 'all' || other.kind === 'all') {
    return one.kind === other.kind;
  }

  return one.mod === other.mod && one.addon === other.addon;
}
