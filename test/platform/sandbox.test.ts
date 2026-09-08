import assert from 'node:assert/strict';
import { test } from 'node:test';
import type { Sandbox } from '../../src/mods/sandbox';
import { openSandbox, type SandboxRuntime } from '../../src/platform/sandbox';

/** The Steam3 account id the boxed Steam signs `second` in as. */
const ACCOUNT_ID = '1547129925';

const SANDBOX: Sandbox = {
  box: 'steam2',
  start: 'C:\\Sandboxie\\Start.exe',
  ini: 'C:\\Sandboxie\\SbieIni.exe',
  steam: 'C:\\Steam',
  account: 'second',
};

test('a stale sign-in does not open the sandbox while the started Steam is still bootstrapping', async () => {
  const runtime = new ControlledRuntime();
  const cancelled = new AbortController();
  const opening = openSandbox(SANDBOX, () => undefined, cancelled.signal, runtime);

  try {
    await runtime.waitForSleep(1);
    assert.equal(runtime.starts, 1);
    const stillWaiting = runtime.waitForSleep(2).then(() => 'waiting' as const);
    runtime.advance(45 * 1000);

    const state = await Promise.race([
      opening.then(
        () => 'settled' as const,
        () => 'settled' as const,
      ),
      stillWaiting,
    ]);

    assert.equal(
      state,
      'waiting',
      'the remembered loginusers.vdf must not release the game before the final Steam client is up',
    );

    runtime.completeLogon();
    runtime.advance(2 * 1000);
    const waitingForClient = await Promise.race([
      opening.then(
        () => 'settled' as const,
        () => 'settled' as const,
      ),
      runtime.waitForSleep(3).then(() => 'waiting' as const),
    ]);
    assert.equal(waitingForClient, 'waiting', 'a completed logon still needs the final Steam client');

    runtime.finishClient();
    runtime.advance(2 * 1000);
    assert.equal(await opening, undefined);
  } finally {
    cancelled.abort();
    await opening.catch(() => undefined);
  }
});

test('an already connected final Steam opens the sandbox immediately', async () => {
  const runtime = new ControlledRuntime('ready');

  assert.equal(await openSandbox(SANDBOX, () => undefined, undefined, runtime), undefined);
  assert.equal(runtime.starts, 0);
  assert.equal(runtime.sleepCount, 0);
});

test('a connected Steam using another remembered account is not ready', async () => {
  const runtime = new ControlledRuntime('ready');
  const cancelled = new AbortController();
  runtime.setRemembered(false);
  const opening = openSandbox(SANDBOX, () => undefined, cancelled.signal, runtime);

  try {
    assert.equal(
      await Promise.race([
        opening.then(
          () => 'settled' as const,
          () => 'settled' as const,
        ),
        runtime.waitForSleep(1).then(() => 'waiting' as const),
      ]),
      'waiting',
    );
    runtime.setRemembered(true);
    runtime.advance(2 * 1000);
    assert.equal(await opening, undefined);
  } finally {
    cancelled.abort();
    await opening.catch(() => undefined);
  }
});

test('an already running Steam cannot reuse a successful logon from its previous process', async () => {
  const runtime = new ControlledRuntime('bootstrap');
  const cancelled = new AbortController();
  const opening = openSandbox(SANDBOX, () => undefined, cancelled.signal, runtime);

  try {
    await runtime.waitForSleep(1);
    const stillWaiting = runtime.waitForSleep(2).then(() => 'waiting' as const);
    runtime.advance(2 * 1000);

    assert.equal(
      await Promise.race([
        opening.then(
          () => 'settled' as const,
          () => 'settled' as const,
        ),
        stillWaiting,
      ]),
      'waiting',
    );
    assert.equal(runtime.starts, 0, 'an existing Steam must be waited for, not started twice');

    runtime.completeLogon();
    runtime.advance(2 * 1000);
    assert.equal(await opening, undefined);
  } finally {
    cancelled.abort();
    await opening.catch(() => undefined);
  }
});

class ControlledRuntime implements SandboxRuntime {
  private clock = new Date(2026, 8, 8, 0, 3, 0).getTime();
  private sleeps = 0;
  private readonly sleepWaiters: { readonly count: number; readonly done: () => void }[] = [];
  private readonly wakeups: (() => void)[] = [];
  private started = false;
  private startedAt: number | undefined;
  private startCalls = 0;
  private finalClient = false;
  private remembered = true;
  private log = loggedOn('2026-09-08 00:01:00');

  constructor(state: 'stopped' | 'bootstrap' | 'ready' = 'stopped') {
    this.started = state !== 'stopped';
    this.startedAt = this.started ? this.clock : undefined;
    this.finalClient = state !== 'stopped';

    if (state === 'ready') {
      this.startedAt = this.clock - 60 * 1000;
      this.completeLogon();
    }
  }

  get starts(): number {
    return this.startCalls;
  }

  get sleepCount(): number {
    return this.sleeps;
  }

  now(): number {
    return this.clock;
  }

  boxExists(): Promise<boolean> {
    return Promise.resolve(true);
  }

  makeBox(): Promise<string | undefined> {
    return Promise.resolve(undefined);
  }

  steamFiles(): Promise<{
    readonly loginUsers: string;
    readonly connectionLogs: readonly string[];
  }> {
    return Promise.resolve({
      loginUsers: 'C:\\Sandbox\\steam2\\drive\\C\\Steam\\config\\loginusers.vdf',
      connectionLogs: [
        'C:\\Sandbox\\steam2\\drive\\C\\Steam\\logs\\connection_log.previous.txt',
        'C:\\Sandbox\\steam2\\drive\\C\\Steam\\logs\\connection_log.txt',
      ],
    });
  }

  imageIsUp(_sandbox: Sandbox, image: string): Promise<boolean> {
    if (image.toLowerCase() === 'steam.exe') {
      return Promise.resolve(this.started);
    }

    return Promise.resolve(image.toLowerCase() === 'steamwebhelper.exe' && this.finalClient);
  }

  imageStartedAt(): Promise<number | undefined> {
    return Promise.resolve(this.startedAt);
  }

  startSteam(): void {
    this.startCalls += 1;
    this.started = true;
    this.startedAt = this.clock;
  }

  accountId(): Promise<string | undefined> {
    return Promise.resolve(this.remembered ? ACCOUNT_ID : undefined);
  }

  readConnectionLogs(): Promise<string> {
    return Promise.resolve(this.log);
  }

  sleep(_milliseconds: number, signal?: AbortSignal): Promise<void> {
    signal?.throwIfAborted();
    this.sleeps += 1;

    for (let index = this.sleepWaiters.length - 1; index >= 0; index -= 1) {
      const waiter = this.sleepWaiters[index];
      if (waiter !== undefined && waiter.count <= this.sleeps) {
        this.sleepWaiters.splice(index, 1);
        waiter.done();
      }
    }

    return new Promise<void>((resolve, reject) => {
      const aborted = () => {
        const reason: unknown = signal?.reason;
        reject(reason instanceof Error ? reason : new Error(String(reason)));
      };
      signal?.addEventListener('abort', aborted, { once: true });
      this.wakeups.push(() => {
        signal?.removeEventListener('abort', aborted);
        resolve();
      });
    });
  }

  waitForSleep(count: number): Promise<void> {
    if (this.sleeps >= count) {
      return Promise.resolve();
    }

    return new Promise((resolve) => this.sleepWaiters.push({ count, done: resolve }));
  }

  advance(milliseconds: number): void {
    const wake = this.wakeups.shift();
    assert.ok(wake, 'the sandbox must be sleeping before its clock advances');
    this.clock += milliseconds;
    wake();
  }

  completeLogon(): void {
    this.log += loggedOn(localTimestamp(this.clock));
  }

  finishClient(): void {
    this.finalClient = true;
  }

  setRemembered(remembered: boolean): void {
    this.remembered = remembered;
  }
}

/** A completed logon by this account, as Steam writes it into the connection log. */
function loggedOn(at: string): string {
  return (
    `[${at}] [Logged On, layered fields] [U:1:${ACCOUNT_ID}] ` +
    'RecvMsgClientLogOnResponse() : processing complete\n'
  );
}

function localTimestamp(at: number): string {
  const date = new Date(at);
  const two = (value: number) => String(value).padStart(2, '0');

  return (
    `${date.getFullYear()}-${two(date.getMonth() + 1)}-${two(date.getDate())} ` +
    `${two(date.getHours())}:${two(date.getMinutes())}:${two(date.getSeconds())}`
  );
}
