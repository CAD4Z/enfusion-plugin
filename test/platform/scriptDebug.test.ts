import assert from 'node:assert/strict';
import { test } from 'node:test';
import type { LaunchRole } from '../../src/mods/launch';
import {
  type ScriptDebugHandler,
  type ScriptDebugPort,
  openScriptDebugPorts,
} from '../../src/platform/scriptDebug';

const HANDLER: ScriptDebugHandler = {
  said: () => undefined,
  note: () => undefined,
};

test('an unexpected multi-port failure closes every listener that did open', async () => {
  const failure = new Error('the server port exploded');
  const calls: LaunchRole[] = [];
  const client = port('client');
  const second = port('client2');

  const opening = openScriptDebugPorts(
    ['client', 'server', 'client2'],
    HANDLER,
    (role) => {
      calls.push(role);

      switch (role) {
        case 'client':
          return Promise.resolve(client);
        case 'server':
          throw failure;
        case 'client2':
          return Promise.resolve(second);
      }
    },
  );

  await assert.rejects(opening, (error: unknown) => error === failure);
  assert.deepEqual(calls, ['client', 'server', 'client2']);
  assert.equal(client.closes, 1);
  assert.equal(second.closes, 1);
});

test('an ordinary unavailable port stays a warning and leaves the other listeners owned', async () => {
  const client = port('client');
  const opened = await openScriptDebugPorts(['client', 'server'], HANDLER, (role) =>
    Promise.resolve(role === 'client' ? client : 'the server port is already in use'),
  );

  assert.deepEqual(opened.warnings, ['the server port is already in use']);
  assert.deepEqual(opened.listening, [client]);
  assert.equal(client.closes, 0);

  client.close();
});

interface ControlledPort extends ScriptDebugPort {
  readonly closes: number;
}

function port(role: LaunchRole): ControlledPort {
  let closes = 0;

  return {
    role,
    port: 1234,
    get closes() {
      return closes;
    },
    close: () => {
      closes += 1;
    },
  };
}
