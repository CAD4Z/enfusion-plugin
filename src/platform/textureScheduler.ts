export type TextureWorkKind = 'inspect' | 'preview' | 'convert' | 'batch';

interface Scheduled<T> {
  readonly kind: TextureWorkKind;
  readonly operation: (signal: AbortSignal) => Promise<T>;
  readonly controller: AbortController;
  readonly external?: AbortSignal;
  readonly resolve: (value: T) => void;
  readonly reject: (reason: Error) => void;
  removeAbort?: () => void;
}

/** One extension-host queue owns every request made to the native texture converter. */
export class TextureScheduler {
  private readonly queued: Scheduled<unknown>[] = [];
  private active?: Scheduled<unknown>;

  run<T>(
    kind: TextureWorkKind,
    operation: (signal: AbortSignal) => Promise<T>,
    signal?: AbortSignal,
  ): Promise<T> {
    if (signal?.aborted === true) {
      return Promise.reject(abortReason(signal));
    }

    return new Promise<T>((resolve, reject) => {
      const task: Scheduled<T> = {
        kind,
        operation,
        controller: new AbortController(),
        external: signal,
        resolve,
        reject,
      };
      if (signal !== undefined) {
        const aborted = (): void => {
          task.controller.abort(abortReason(signal));
          const at = this.queued.indexOf(task as Scheduled<unknown>);
          if (at >= 0) {
            this.queued.splice(at, 1);
            task.removeAbort?.();
            reject(abortReason(signal));
          }
        };
        signal.addEventListener('abort', aborted, { once: true });
        task.removeAbort = () => signal.removeEventListener('abort', aborted);
      }

      if (isHeavy(kind)) this.cancelQueuedPreviews();
      this.queued.push(task as Scheduled<unknown>);
      this.queued.sort((left, right) => priorityOf(right.kind) - priorityOf(left.kind));
      if (isHeavy(kind) && this.active?.kind === 'preview') {
        this.active.controller.abort(new Error('Preview superseded by conversion work.'));
      }
      this.pump();
    });
  }

  private cancelQueuedPreviews(): void {
    for (let at = this.queued.length - 1; at >= 0; at -= 1) {
      const task = this.queued[at];
      if (task?.kind !== 'preview') continue;
      this.queued.splice(at, 1);
      const reason = new Error('Preview superseded by conversion work.');
      task.controller.abort(reason);
      task.removeAbort?.();
      task.reject(reason);
    }
  }

  private pump(): void {
    if (this.active !== undefined) return;
    const task = this.queued.shift();
    if (task === undefined) return;
    this.active = task;
    if (task.external?.aborted === true) {
      this.finished(task, false, abortReason(task.external));
      return;
    }

    void task.operation(task.controller.signal).then(
      (value) => this.finished(task, true, value),
      (error: unknown) => this.finished(task, false, error),
    );
  }

  private finished(task: Scheduled<unknown>, succeeded: boolean, value: unknown): void {
    task.removeAbort?.();
    if (this.active === task) this.active = undefined;
    if (succeeded) task.resolve(value);
    else task.reject(errorOf(value));
    this.pump();
  }
}

function priorityOf(kind: TextureWorkKind): number {
  return isHeavy(kind) ? 2 : kind === 'inspect' ? 1 : 0;
}

function isHeavy(kind: TextureWorkKind): boolean {
  return kind === 'convert' || kind === 'batch';
}

function abortReason(signal: AbortSignal): Error {
  return errorOf(signal.reason ?? 'Texture work was cancelled.');
}

function errorOf(value: unknown): Error {
  return value instanceof Error ? value : new Error(String(value));
}
