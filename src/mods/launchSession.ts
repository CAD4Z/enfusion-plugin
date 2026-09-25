/**
 * The lifetime of a launch, apart from the debug protocol that presents it.
 *
 * A starter owns everything it acquires until it returns one `ActiveLaunch`. If it cannot return
 * one, it rolls its partial work back. From that return onward this module is the sole owner: Stop,
 * a primary process going away, and disposal all travel through the same idempotent path.
 *
 * There is one coordinator for a workspace. That makes single flight true for every way into Run
 * and Debug rather than only for the panel button: two sessions cannot prepare the same run folder
 * or put two servers on the same port beside each other.
 */

import type { LaunchRole } from './launch';

/** How a process that had successfully spawned eventually went away. */
export type ProcessExit =
  | { readonly kind: 'code'; readonly code: number }
  | { readonly kind: 'signal'; readonly signal: string }
  | { readonly kind: 'unknown' };

/** The first process of an active launch that went away, and how. */
export interface LaunchExit {
  readonly role: LaunchRole;
  readonly outcome: ProcessExit;
}

/** The process handle needed to own a launch; platform-specific details stay behind this seam. */
export interface LaunchGame {
  readonly role: LaunchRole;
  /** Never rejects. A failure to spawn is reported before a `LaunchGame` is returned. */
  readonly exited: Promise<ProcessExit>;
  /** Takes the process and its children down. Doing it twice is not an error. */
  kill(): Promise<void>;
}

/** A listener held for as long as the games whose script log it carries. */
export interface LaunchListener {
  /** Drops connections and stops listening. Doing it twice is not an error. */
  close(): void;
}

/**
 * One atomic piece of a running session: its processes and every listener they need.
 *
 * `ended` never rejects and resolves for the first process to leave. `stop` is idempotent, attempts
 * every cleanup even where one fails, and returns those failures rather than throwing past the
 * cleanup of the rest.
 */
export interface ActiveLaunch {
  readonly ended: Promise<LaunchExit>;
  stop(): Promise<readonly unknown[]>;
}

/** Bundles platform handles into the one value whose ownership a starter can transfer. */
export function activeLaunchOf(
  games: readonly LaunchGame[],
  listeners: readonly LaunchListener[],
): ActiveLaunch {
  const ownedGames = [...games];
  const ownedListeners = [...listeners];
  const ended = Promise.race(
    ownedGames.map(async (game): Promise<LaunchExit> => ({
      role: game.role,
      outcome: await game.exited,
    })),
  );
  let stopping: Promise<readonly unknown[]> | undefined;

  return {
    ended,
    stop: () => {
      stopping ??= stopAll(ownedGames, ownedListeners);
      return stopping;
    },
  };
}

async function stopAll(
  games: readonly LaunchGame[],
  listeners: readonly LaunchListener[],
): Promise<readonly unknown[]> {
  const failures: unknown[] = [];

  // Ports are released before processes are killed. A process still on its way down may try to
  // reconnect, but a launch that is over has no console left for that connection to belong to.
  for (const listener of listeners) {
    try {
      listener.close();
    } catch (error: unknown) {
      failures.push(error);
    }
  }

  const killed = await Promise.allSettled(games.map(async (game) => game.kill()));
  for (const outcome of killed) {
    if (outcome.status === 'rejected') {
      failures.push(outcome.reason);
    }
  }

  return failures;
}

/** A transactional starter: refusal owns nothing, success transfers one whole active launch. */
export type LaunchAttempt =
  | { readonly kind: 'started'; readonly launch: ActiveLaunch }
  | { readonly kind: 'refused'; readonly message: string };

/**
 * Starts one atomic launch while the signal remains live.
 *
 * It must honour cancellation before every process spawn. If it throws or returns a refusal, it
 * still owns and must stop anything it acquired. Once it returns `started`, ownership has moved to
 * `LaunchSession` and the starter must not touch the launch again.
 */
export type StartLaunch = (signal: AbortSignal) => Promise<LaunchAttempt>;

/** What one request to start a primary launch or a second client amounted to. */
export type LaunchStartOutcome =
  | { readonly kind: 'started' }
  | { readonly kind: 'refused'; readonly message: string }
  | { readonly kind: 'failed'; readonly error: unknown }
  /** Another debug session owns this workspace's launch slot. */
  | { readonly kind: 'busy' }
  /** This debug session has already accepted its one primary launch request. */
  | { readonly kind: 'already-started' }
  /** Stop won a race with this request; any late launch has already been taken down. */
  | { readonly kind: 'inactive' };

/** Natural lifecycle events. Protocol-driven termination is reported by the caller that asked it. */
export interface LaunchSessionEvents {
  /** A primary process left, the rest were stopped, and the session is now over. */
  ended(exit: LaunchExit): void;
  /** The second client left and its slot is free again; the primary launch is still up. */
  secondEnded(exit: LaunchExit): void;
  /** Cleanup carried on after this failure; the failure is for the log rather than control flow. */
  cleanupFailed(error: unknown): void;
}

/** The small interface through which a debug adapter drives one launch session. */
export interface LaunchSession {
  start(starter: StartLaunch): Promise<LaunchStartOutcome>;
  addSecond(starter: StartLaunch): Promise<LaunchStartOutcome>;
  /** Answers whether this call was the one that began terminal cleanup. */
  stop(): Promise<boolean>;
}

/** One workspace-wide gate, and the factory for sessions that share it. */
export class LaunchCoordinator {
  private owner: CoordinatedLaunchSession | undefined;

  /** True through startup, runtime and cleanup, including a cancelled starter rolling back. */
  get busy(): boolean {
    return this.owner !== undefined;
  }

  /** A process is starting or running; false while an old launch is only cleaning itself up. */
  get active(): boolean {
    return this.owner?.active ?? false;
  }

  session(events: LaunchSessionEvents): LaunchSession {
    const session = new CoordinatedLaunchSession(
      events,
      () => {
        if (this.owner !== undefined) {
          return false;
        }
        this.owner = session;
        return true;
      },
      () => {
        if (this.owner === session) {
          this.owner = undefined;
        }
      },
    );

    return session;
  }

  /**
   * Finishes the launch that still owns the slot after VS Code has already lost its debug session.
   * The caller must establish that there is no live Enfusion session before using this recovery
   * path; a running session remains the authority for its own Stop button.
   */
  async stopOrphan(): Promise<boolean> {
    const owner = this.owner;
    return owner === undefined ? false : owner.stop();
  }
}

type SessionState = 'new' | 'starting' | 'running' | 'stopping' | 'stopped';

class CoordinatedLaunchSession implements LaunchSession {
  private state: SessionState = 'new';
  private readonly cancellation = new AbortController();
  private primary: ActiveLaunch | undefined;
  private second: ActiveLaunch | undefined;
  private secondStarting = false;
  private stopping: Promise<void> | undefined;
  private pending = 0;
  private claimed = false;

  constructor(
    private readonly events: LaunchSessionEvents,
    private readonly claim: () => boolean,
    private readonly release: () => void,
  ) {}

  get active(): boolean {
    return this.state === 'starting' || this.state === 'running';
  }

  async start(starter: StartLaunch): Promise<LaunchStartOutcome> {
    if (this.state !== 'new') {
      return { kind: 'already-started' };
    }

    if (!this.claim()) {
      this.state = 'stopped';
      this.cancellation.abort();
      return { kind: 'busy' };
    }

    this.claimed = true;
    this.state = 'starting';

    return this.pendingWhile(async () => {
      let attempt: LaunchAttempt;

      try {
        attempt = await starter(this.cancellation.signal);
      } catch (error: unknown) {
        if (this.state !== 'starting' || this.cancellation.signal.aborted) {
          return { kind: 'inactive' };
        }

        await this.stop();
        return { kind: 'failed', error };
      }

      if (attempt.kind === 'refused') {
        if (this.state !== 'starting') {
          return { kind: 'inactive' };
        }

        await this.stop();
        return { kind: 'refused', message: attempt.message };
      }

      if (this.state !== 'starting') {
        await this.stopActive(attempt.launch);
        return { kind: 'inactive' };
      }

      this.primary = attempt.launch;
      this.state = 'running';
      this.watchPrimary(attempt.launch);
      return { kind: 'started' };
    });
  }

  async addSecond(starter: StartLaunch): Promise<LaunchStartOutcome> {
    if (this.state !== 'running') {
      return { kind: 'inactive' };
    }

    if (this.secondStarting || this.second !== undefined) {
      return { kind: 'busy' };
    }

    this.secondStarting = true;

    return this.pendingWhile(async () => {
      try {
        let attempt: LaunchAttempt;

        try {
          attempt = await starter(this.cancellation.signal);
        } catch (error: unknown) {
          return this.state !== 'running' || this.cancellation.signal.aborted
            ? { kind: 'inactive' }
            : { kind: 'failed', error };
        }

        if (attempt.kind === 'refused') {
          return this.state === 'running'
            ? { kind: 'refused', message: attempt.message }
            : { kind: 'inactive' };
        }

        if (this.state !== 'running') {
          await this.stopActive(attempt.launch);
          return { kind: 'inactive' };
        }

        this.second = attempt.launch;
        this.watchSecond(attempt.launch);
        return { kind: 'started' };
      } finally {
        this.secondStarting = false;
      }
    });
  }

  async stop(): Promise<boolean> {
    if (this.stopping !== undefined) {
      await this.stopping;
      return false;
    }

    if (this.state === 'stopped') {
      return false;
    }

    this.cancellation.abort();
    this.state = 'stopping';

    const active = [this.primary, this.second].filter(
      (launch): launch is ActiveLaunch => launch !== undefined,
    );
    this.primary = undefined;
    this.second = undefined;
    this.stopping = Promise.all(active.map(async (launch) => this.stopActive(launch))).then(
      () => undefined,
    );

    await this.stopping;
    this.state = 'stopped';
    this.releaseWhenSettled();
    return true;
  }

  private watchPrimary(launch: ActiveLaunch): void {
    void launch.ended
      .then(async (exit) => {
        if (this.state !== 'running' || this.primary !== launch) {
          return;
        }

        if (await this.stop()) {
          this.tell(() => this.events.ended(exit));
        }
      })
      .catch((error: unknown) => this.report(error));
  }

  private watchSecond(launch: ActiveLaunch): void {
    void launch.ended
      .then(async (exit) => {
        if (this.state !== 'running' || this.second !== launch) {
          return;
        }

        await this.stopActive(launch);
        if (this.state !== 'running' || this.second !== launch) {
          return;
        }

        this.second = undefined;
        this.tell(() => this.events.secondEnded(exit));
      })
      .catch((error: unknown) => this.report(error));
  }

  private async stopActive(launch: ActiveLaunch): Promise<void> {
    try {
      for (const failure of await launch.stop()) {
        this.report(failure);
      }
    } catch (error: unknown) {
      // Adapters are required to return cleanup failures, but a broken one still must not strand
      // the session in `stopping` or keep the workspace-wide gate forever.
      this.report(error);
    }
  }

  private async pendingWhile<T>(work: () => Promise<T>): Promise<T> {
    this.pending += 1;

    try {
      return await work();
    } finally {
      this.pending -= 1;
      this.releaseWhenSettled();
    }
  }

  /** A cancelled starter owns its partial work until it settles, so the next launch waits for it. */
  private releaseWhenSettled(): void {
    if (this.claimed && this.state === 'stopped' && this.pending === 0) {
      this.claimed = false;
      this.release();
    }
  }

  private report(error: unknown): void {
    this.tell(() => this.events.cleanupFailed(error));
  }

  private tell(event: () => void): void {
    try {
      event();
    } catch {
      // An observer is outside the lifetime it observes and cannot be allowed to break cleanup.
    }
  }
}
