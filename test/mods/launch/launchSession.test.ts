import assert from 'node:assert/strict';
import { test } from 'node:test';
import type { LaunchRole } from '../../../src/mods/launch/launch';
import {
  type ActiveLaunch,
  type LaunchExit,
  type LaunchGame,
  type LaunchListener,
  type LaunchSessionEvents,
  type ProcessExit,
  LaunchCoordinator,
  activeLaunchOf,
} from '../../../src/mods/launch/launchSession';

const EXIT_ZERO: ProcessExit = { kind: 'code', code: 0 };

test('an active launch ends with its first process and stops every resource once', async () => {
  const server = game('server', new Error('server would not stop'));
  const client = game('client');
  const port = listener(new Error('port would not close'));
  const active = activeLaunchOf([server, client], [port]);

  client.leave({ kind: 'signal', signal: 'SIGTERM' });
  assert.deepEqual(await active.ended, {
    role: 'client',
    outcome: { kind: 'signal', signal: 'SIGTERM' },
  });

  const [first, second] = await Promise.all([active.stop(), active.stop()]);

  assert.equal(port.closes, 1);
  assert.equal(server.kills, 1);
  assert.equal(client.kills, 1);
  assert.equal(first.length, 2);
  assert.strictEqual(second, first);

  await active.stop();
  assert.equal(port.closes, 1);
  assert.equal(server.kills, 1);
  assert.equal(client.kills, 1);
});

test('the coordinator gives every entry point one workspace-wide launch slot', async () => {
  const coordinator = new LaunchCoordinator();
  const first = coordinator.session(events());
  const other = coordinator.session(events());
  const primary = controlledLaunch('client');
  let otherStarterCalls = 0;

  assert.deepEqual(await first.start(started(primary.active)), { kind: 'started' });
  assert.equal(coordinator.busy, true);
  assert.deepEqual(
    await other.start(() => {
      otherStarterCalls += 1;
      return Promise.resolve({
        kind: 'started',
        launch: controlledLaunch('client').active,
      } as const);
    }),
    { kind: 'busy' },
  );
  assert.equal(otherStarterCalls, 0);

  assert.equal(await first.stop(), true);
  assert.equal(await first.stop(), false);
  assert.equal(coordinator.busy, false);

  const next = coordinator.session(events());
  assert.deepEqual(await next.start(started(controlledLaunch('client').active)), {
    kind: 'started',
  });
  await next.stop();
});

test('an orphaned adapter can be stopped before a fresh panel launch claims the slot', async () => {
  const coordinator = new LaunchCoordinator();
  const orphan = coordinator.session(events());
  const primary = controlledLaunch('client');

  await orphan.start(started(primary.active));
  assert.equal(coordinator.busy, true);

  assert.equal(await coordinator.stopOrphan(), true);
  assert.equal(primary.process.kills, 1);
  assert.equal(primary.listener.closes, 1);
  assert.equal(coordinator.busy, false);
  assert.equal(await coordinator.stopOrphan(), false);
});

test('a launch in cleanup is busy but no longer active', async () => {
  const coordinator = new LaunchCoordinator();
  const session = coordinator.session(events());
  const exit = deferred<LaunchExit>();
  const cleanup = deferred<readonly unknown[]>();

  await session.start(() =>
    Promise.resolve({
      kind: 'started',
      launch: { ended: exit.promise, stop: () => cleanup.promise },
    }),
  );
  assert.equal(coordinator.active, true);

  exit.resolve({ role: 'client', outcome: EXIT_ZERO });
  await turn();
  assert.equal(coordinator.busy, true);
  assert.equal(coordinator.active, false);

  const finishing = coordinator.stopOrphan();
  cleanup.resolve([]);
  assert.equal(await finishing, false);
  assert.equal(coordinator.busy, false);
});

test('a refused or failed primary start ends its session and releases the slot', async () => {
  const coordinator = new LaunchCoordinator();
  const refused = coordinator.session(events());

  assert.deepEqual(
    await refused.start(() => Promise.resolve({ kind: 'refused', message: 'not built' })),
    { kind: 'refused', message: 'not built' },
  );
  assert.equal(coordinator.busy, false);

  const failed = coordinator.session(events());
  const failure = new Error('could not prepare the run folder');
  const outcome = await failed.start(() => Promise.reject(failure));

  assert.deepEqual(outcome, { kind: 'failed', error: failure });
  assert.equal(coordinator.busy, false);
});

test('a primary launch request is accepted only once by one session', async () => {
  const coordinator = new LaunchCoordinator();
  const session = coordinator.session(events());
  let calls = 0;

  const starter = () => {
    calls += 1;
    return Promise.resolve({
      kind: 'started',
      launch: controlledLaunch('client').active,
    } as const);
  };

  assert.deepEqual(await session.start(starter), { kind: 'started' });
  assert.deepEqual(await session.start(starter), { kind: 'already-started' });
  assert.equal(calls, 1);
  await session.stop();
});

test('Stop during primary startup aborts it and takes down a late result', async () => {
  const coordinator = new LaunchCoordinator();
  const session = coordinator.session(events());
  const attempt = deferred<ActiveLaunch>();
  const late = controlledLaunch('client');
  let signal: AbortSignal | undefined;

  const starting = session.start(async (given) => {
    signal = given;
    return { kind: 'started', launch: await attempt.promise };
  });
  await turn();

  assert.equal(await session.stop(), true);
  assert.equal(signal?.aborted, true);
  // The starter still owns a result that has not come back, so another session cannot race it in
  // the run folder even though the debug session itself has ended.
  assert.equal(coordinator.busy, true);

  attempt.resolve(late.active);
  assert.deepEqual(await starting, { kind: 'inactive' });
  assert.equal(late.process.kills, 1);
  assert.equal(late.listener.closes, 1);
  assert.equal(coordinator.busy, false);
});

test('the first primary process to exit ends the session and stops every sibling', async () => {
  const coordinator = new LaunchCoordinator();
  const heard = events();
  const session = coordinator.session(heard);
  const server = game('server');
  const client = game('client');
  const primaryPort = listener();
  const primary = activeLaunchOf([server, client], [primaryPort]);
  const second = controlledLaunch('client2');

  await session.start(started(primary));
  await session.addSecond(started(second.active));

  server.leave(EXIT_ZERO);
  await eventually(() => heard.ends.length === 1);

  assert.deepEqual(heard.ends, [{ role: 'server', outcome: EXIT_ZERO }]);
  assert.deepEqual(heard.secondEnds, []);
  assert.equal(server.kills, 1);
  assert.equal(client.kills, 1);
  assert.equal(second.process.kills, 1);
  assert.equal(primaryPort.closes, 1);
  assert.equal(second.listener.closes, 1);
  assert.equal(coordinator.busy, false);
  assert.equal(await session.stop(), false);
});

test('manual Stop wins cleanly over process exit without a second terminal event', async () => {
  const heard = events();
  const session = new LaunchCoordinator().session(heard);
  const primary = controlledLaunch('client');

  await session.start(started(primary.active));
  primary.process.leave(EXIT_ZERO);
  assert.equal(await session.stop(), true);
  await turn();

  assert.deepEqual(heard.ends, []);
  assert.equal(primary.process.kills, 1);
  assert.equal(primary.listener.closes, 1);
});

test('only one second client may start or run, and its exit frees the slot', async () => {
  const heard = events();
  const session = new LaunchCoordinator().session(heard);
  const primary = controlledLaunch('client');
  const pending = deferred<ActiveLaunch>();
  const second = controlledLaunch('client2');

  await session.start(started(primary.active));
  const starting = session.addSecond(async () => ({
    kind: 'started',
    launch: await pending.promise,
  }));
  await turn();

  assert.deepEqual(await session.addSecond(started(controlledLaunch('client2').active)), {
    kind: 'busy',
  });

  pending.resolve(second.active);
  assert.deepEqual(await starting, { kind: 'started' });
  assert.deepEqual(await session.addSecond(started(controlledLaunch('client2').active)), {
    kind: 'busy',
  });

  second.process.leave({ kind: 'code', code: 3 });
  await eventually(() => heard.secondEnds.length === 1);

  assert.deepEqual(heard.secondEnds, [
    { role: 'client2', outcome: { kind: 'code', code: 3 } },
  ]);
  assert.equal(second.process.kills, 1);
  assert.equal(second.listener.closes, 1);
  assert.equal(primary.process.kills, 0);

  const replacement = controlledLaunch('client2');
  assert.deepEqual(await session.addSecond(started(replacement.active)), { kind: 'started' });
  await session.stop();
  assert.equal(replacement.process.kills, 1);
});

test('a refused or failed second client leaves the primary launch alone', async () => {
  const coordinator = new LaunchCoordinator();
  const session = coordinator.session(events());
  const primary = controlledLaunch('client');
  const failure = new Error('Sandboxie is unavailable');

  await session.start(started(primary.active));
  assert.deepEqual(
    await session.addSecond(() =>
      Promise.resolve({ kind: 'refused', message: 'sign in first' }),
    ),
    { kind: 'refused', message: 'sign in first' },
  );
  assert.deepEqual(
    await session.addSecond(() => Promise.reject(failure)),
    { kind: 'failed', error: failure },
  );

  assert.equal(primary.process.kills, 0);
  assert.equal(coordinator.busy, true);
  await session.stop();
});

test('Stop aborts a pending second client and suppresses its late exit message', async () => {
  const heard = events();
  const coordinator = new LaunchCoordinator();
  const session = coordinator.session(heard);
  const primary = controlledLaunch('client');
  const pending = deferred<ActiveLaunch>();
  const late = controlledLaunch('client2');
  let signal: AbortSignal | undefined;

  await session.start(started(primary.active));
  const starting = session.addSecond(async (given) => {
    signal = given;
    return { kind: 'started', launch: await pending.promise };
  });
  await turn();

  await session.stop();
  assert.equal(signal?.aborted, true);
  assert.equal(coordinator.busy, true);

  pending.resolve(late.active);
  assert.deepEqual(await starting, { kind: 'inactive' });
  late.process.leave(EXIT_ZERO);
  await turn();

  assert.deepEqual(heard.secondEnds, []);
  assert.equal(late.process.kills, 1);
  assert.equal(late.listener.closes, 1);
  assert.equal(coordinator.busy, false);
});

test('cleanup failures are reported but do not strand the session or its other resources', async () => {
  const heard = events();
  const coordinator = new LaunchCoordinator();
  const session = coordinator.session(heard);
  const badKill = new Error('taskkill failed');
  const badClose = new Error('close failed');
  const failing = game('server', badKill);
  const sibling = game('client');
  const primary = activeLaunchOf([failing, sibling], [listener(badClose)]);

  await session.start(started(primary));
  failing.leave(EXIT_ZERO);
  await eventually(() => heard.ends.length === 1);

  assert.equal(failing.kills, 1);
  assert.equal(sibling.kills, 1);
  assert.deepEqual(heard.failures, [badClose, badKill]);
  assert.equal(coordinator.busy, false);
});

function started(launch: ActiveLaunch) {
  return () => Promise.resolve({ kind: 'started', launch } as const);
}

interface ControlledGame extends LaunchGame {
  readonly kills: number;
  leave(exit: ProcessExit): void;
}

function game(role: LaunchRole, killFailure?: Error): ControlledGame {
  const exit = deferred<ProcessExit>();
  let kills = 0;

  return {
    role,
    exited: exit.promise,
    get kills() {
      return kills;
    },
    leave: (outcome) => exit.resolve(outcome),
    kill: () => {
      kills += 1;
      if (killFailure !== undefined) {
        return Promise.reject(killFailure);
      }

      return Promise.resolve();
    },
  };
}

interface ControlledListener extends LaunchListener {
  readonly closes: number;
}

function listener(closeFailure?: Error): ControlledListener {
  let closes = 0;

  return {
    get closes() {
      return closes;
    },
    close: () => {
      closes += 1;
      if (closeFailure !== undefined) {
        throw closeFailure;
      }
    },
  };
}

function controlledLaunch(role: LaunchRole): {
  readonly active: ActiveLaunch;
  readonly process: ControlledGame;
  readonly listener: ControlledListener;
} {
  const process = game(role);
  const port = listener();

  return { active: activeLaunchOf([process], [port]), process, listener: port };
}

interface Heard extends LaunchSessionEvents {
  readonly ends: LaunchExit[];
  readonly secondEnds: LaunchExit[];
  readonly failures: unknown[];
}

function events(): Heard {
  const ends: LaunchExit[] = [];
  const secondEnds: LaunchExit[] = [];
  const failures: unknown[] = [];

  return {
    ends,
    secondEnds,
    failures,
    ended: (exit) => ends.push(exit),
    secondEnded: (exit) => secondEnds.push(exit),
    cleanupFailed: (error) => failures.push(error),
  };
}

function deferred<T>(): { readonly promise: Promise<T>; resolve(value: T): void } {
  let resolve = (_value: T): void => {
    throw new Error('the deferred promise was not initialised');
  };
  const promise = new Promise<T>((done) => {
    resolve = done;
  });

  return { promise, resolve };
}

function turn(): Promise<void> {
  return new Promise((resolve) => setImmediate(resolve));
}

async function eventually(done: () => boolean): Promise<void> {
  for (let attempt = 0; attempt < 20; attempt += 1) {
    if (done()) {
      return;
    }
    await turn();
  }

  assert.fail('the lifecycle event did not arrive');
}
