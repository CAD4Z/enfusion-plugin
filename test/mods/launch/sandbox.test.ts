import assert from 'node:assert/strict';
import { test } from 'node:test';
import type { SecondClient } from '../../../src/mods/machine';
import {
  BOX,
  BOX_SETTINGS,
  boxExistsOf,
  boxPidsOf,
  boxPrefixOf,
  boxRootOf,
  connectionLogPathOf,
  gamePrefixOf,
  imagePidsOf,
  inBoxOf,
  loginUsersPathOf,
  previousConnectionLogPathOf,
  SIGN_IN_PATIENCE,
  sandboxPlanOf,
  steamAccountIdOf,
  steamConnectedSinceOf,
  steamCommandOf,
  steamConfigPathOf,
  steamExecutableOf,
} from '../../../src/mods/launch/sandbox';

/** The Steam3 account id of the account the fixtures below are signed in as. */
const ACCOUNT_ID = '123';

const SECOND: SecondClient = {
  account: 'estrv05733',
  sandboxie: 'C:\\Program Files\\Sandboxie-Plus',
  steam: 'C:\\Program Files (x86)\\Steam',
};

test('a first Steam sign-in is given five minutes', () => {
  assert.equal(SIGN_IN_PATIENCE, 5 * 60 * 1000);
});

test('an account with Sandboxie and Steam behind it is a box', () => {
  const plan = sandboxPlanOf(SECOND);

  assert.deepEqual(plan, {
    kind: 'box',
    sandbox: {
      box: BOX,
      start: 'C:\\Program Files\\Sandboxie-Plus\\Start.exe',
      ini: 'C:\\Program Files\\Sandboxie-Plus\\SbieIni.exe',
      steam: 'C:\\Program Files (x86)\\Steam',
      account: 'estrv05733',
    },
  });
});

test('no account is a second client that is simply another client', () => {
  assert.deepEqual(sandboxPlanOf({ ...SECOND, account: '' }), { kind: 'none' });
});

test('an account with nowhere to run it says what to install', () => {
  const plan = sandboxPlanOf({ ...SECOND, sandboxie: '' });

  assert.equal(plan.kind, 'wanting');
  assert.match(plan.kind === 'wanting' ? plan.said : '', /Sandboxie/);
});

test('an account with no Steam behind it says which setting fills it in', () => {
  const plan = sandboxPlanOf({ ...SECOND, steam: '' });

  assert.equal(plan.kind, 'wanting');
  assert.match(plan.kind === 'wanting' ? plan.said : '', /enfusion\.steam\.path/);
});

test('the game waits in the box, and Steam skips the bootstrap update that cannot run there', () => {
  const plan = sandboxPlanOf(SECOND);
  assert.equal(plan.kind, 'box');
  if (plan.kind !== 'box') {
    return;
  }

  assert.deepEqual(boxPrefixOf(plan.sandbox), [
    'C:\\Program Files\\Sandboxie-Plus\\Start.exe',
    '/box:steam2',
  ]);
  assert.deepEqual(gamePrefixOf(plan.sandbox), [
    'C:\\Program Files\\Sandboxie-Plus\\Start.exe',
    '/box:steam2',
    '/wait',
  ]);
  assert.equal(steamExecutableOf(plan.sandbox), 'C:\\Program Files (x86)\\Steam\\steam.exe');
  assert.deepEqual(steamCommandOf(plan.sandbox), [
    'C:\\Program Files (x86)\\Steam\\steam.exe',
    '-login',
    'estrv05733',
    '-silent',
    '-inhibitbootstrap',
  ]);
});

test('a box is made enabled first, and kept from being deleted', () => {
  assert.deepEqual(BOX_SETTINGS[0], ['Enabled', 'y']);
  assert.deepEqual([...BOX_SETTINGS], [
    ['Enabled', 'y'],
    ['NeverRemove', 'y'],
    ['AutoDelete', 'n'],
  ]);
});

test('a box that is not there is answered with nothing', () => {
  assert.equal(boxExistsOf('y\r\n'), true);
  assert.equal(boxExistsOf(''), false);
  assert.equal(boxExistsOf('\r\n'), false);
});

test('the count in front of the pids is not one of them', () => {
  assert.deepEqual(boxPidsOf('3\r\n28652\r\n12584\r\n18256\r\n'), [28652, 12584, 18256]);
  assert.deepEqual(boxPidsOf('0\r\n'), []);
  assert.deepEqual(boxPidsOf(''), []);
});

test('the pids of a program are read out of the quoted rows and nothing else', () => {
  // tasklist on a Russian Windows: "INFO: No tasks are running which match the specified
  // criteria.", and sizes in KB.
  const info =
    '\u0418\u041d\u0424\u041e: \u043d\u0435\u0442 ' +
    '\u0437\u0430\u043f\u0443\u0449\u0435\u043d\u043d\u044b\u0445 ' +
    '\u0437\u0430\u0434\u0430\u0447, ' +
    '\u0441\u043e\u043e\u0442\u0432\u0435\u0442\u0441\u0442\u0432\u0443\u044e\u0449\u0438\u0445 ' +
    '\u0443\u043a\u0430\u0437\u0430\u043d\u043d\u044b\u043c ' +
    '\u043a\u0440\u0438\u0442\u0435\u0440\u0438\u044f\u043c.\r\n';
  const said =
    info +
    '"steam.exe","7488","Console","1","108 192 \u041a\u0411"\r\n' +
    '"steam.exe","28652","Console","1","144 320 \u041a\u0411"\r\n';

  assert.deepEqual(imagePidsOf(said), [7488, 28652]);
  assert.deepEqual(imagePidsOf(info), []);
});

test('a box root nobody configured is the one Sandboxie would have used', () => {
  assert.equal(boxRootOf('', 'steam2', 'Ilya', 'C:'), 'C:\\Sandbox\\Ilya\\steam2');
  assert.equal(
    boxRootOf('\\??\\%SystemDrive%\\Sandbox\\%USER%\\%SANDBOX%\r\n', 'steam2', 'Ilya', 'D:'),
    'D:\\Sandbox\\Ilya\\steam2',
  );
});

test('a box root that names no box gets the box put under it', () => {
  assert.equal(boxRootOf('E:\\Sandboxes', 'steam2', 'Ilya', 'C:'), 'E:\\Sandboxes\\steam2');
});

test('the box holds Steam’s record of the sign-in under a folder per drive letter', () => {
  assert.equal(
    loginUsersPathOf('C:\\Sandbox\\Ilya\\steam2', 'C:\\Program Files (x86)\\Steam'),
    'C:\\Sandbox\\Ilya\\steam2\\drive\\C\\Program Files (x86)\\Steam\\config\\loginusers.vdf',
  );
  assert.equal(loginUsersPathOf('C:\\Sandbox\\Ilya\\steam2', '\\\\server\\Steam'), undefined);
  assert.equal(
    steamConfigPathOf('C:\\Sandbox\\Ilya\\steam2', 'C:\\Program Files (x86)\\Steam'),
    'C:\\Sandbox\\Ilya\\steam2\\drive\\C\\Program Files (x86)\\Steam\\config\\config.vdf',
  );
});

/**
 * A second client's display settings, the way the box keeps them once the game has written them:
 * under the drive the work drive is mounted from, since the path handed in is already the real one.
 */
test('the box keeps its copy of any file on a drive under that drive letter', () => {
  assert.equal(
    inBoxOf(
      'C:\\Sandbox\\Ilya\\steam2',
      'F:\\Code\\DayZ\\PDrive\\Profiles\\CADCore\\client2\\Users\\Ilya\\DayZ.cfg',
    ),
    'C:\\Sandbox\\Ilya\\steam2\\drive\\F\\Code\\DayZ\\PDrive\\Profiles\\CADCore\\client2\\Users\\Ilya\\DayZ.cfg',
  );
  assert.equal(inBoxOf('C:\\Sandbox\\Ilya\\steam2', '\\\\server\\share\\DayZ.cfg'), undefined);
});

test('the box holds Steam’s connection log beside the rest of its installation', () => {
  assert.equal(
    connectionLogPathOf('C:\\Sandbox\\Ilya\\steam2', 'C:\\Program Files (x86)\\Steam'),
    'C:\\Sandbox\\Ilya\\steam2\\drive\\C\\Program Files (x86)\\Steam\\logs\\connection_log.txt',
  );
  assert.equal(
    previousConnectionLogPathOf('C:\\Sandbox\\Ilya\\steam2', 'C:\\Program Files (x86)\\Steam'),
    'C:\\Sandbox\\Ilya\\steam2\\drive\\C\\Program Files (x86)\\Steam\\logs\\connection_log.previous.txt',
  );
  assert.equal(connectionLogPathOf('C:\\Sandbox\\Ilya\\steam2', '\\\\server\\Steam'), undefined);
});

test('the account id in what Steam wrote is what a connection is read against', () => {
  const vdf = `"users"
{
\t"76561197960265851"
\t{
\t\t"AccountName"\t\t"estrv05733"
\t\t"AutoLogin"\t\t"1"
\t}
}`;

  assert.equal(steamAccountIdOf(vdf, 'estrv05733'), ACCOUNT_ID);
  assert.equal(steamAccountIdOf(vdf, 'ESTRV05733'), ACCOUNT_ID);
  assert.equal(steamAccountIdOf(vdf, 'thehurfy'), undefined);
  assert.equal(steamAccountIdOf('', 'estrv05733'), undefined);
});

test('every account the box remembers keeps an id of its own', () => {
  const vdf = `"users"
{
\t"76561197960265729"
\t{
\t\t"AccountName" "first"
\t}
\t"76561197960265730"
\t{
\t\t"AccountName" "second"
\t}
}`;

  assert.equal(steamAccountIdOf(vdf, 'first'), '1');
  assert.equal(steamAccountIdOf(vdf, 'second'), '2');
});

test('config.vdf identifies a signed-in account even before loginusers.vdf is written', () => {
  const vdf = `"InstallConfigStore" {
    "Software" { "Valve" { "Steam" { "Accounts" {
      "first" { "SteamID" "76561197960265729" }
      "second" { "SteamID" "76561197960265851" }
      "invalid" { "SteamID" "pending" }
    } } } }
  }`;

  assert.equal(steamAccountIdOf(vdf, 'second'), ACCOUNT_ID);
  assert.equal(steamAccountIdOf(vdf, 'SECOND'), ACCOUNT_ID);
  assert.equal(steamAccountIdOf(vdf, 'first'), '1');
  assert.equal(steamAccountIdOf(vdf, 'absent'), undefined);
  assert.equal(steamAccountIdOf(vdf, 'invalid'), undefined);
  assert.equal(steamAccountIdOf('"second" { "SteamID" "76561197960265851" }', 'second'), undefined);
});

test('Steam is connected only after its latest session finishes logging on', () => {
  const started = new Date(2026, 8, 8, 0, 0, 0).getTime();
  const oldSession = loggedOn('00:01:00');
  const denied =
    '[2026-09-08 00:02:00] [Logging On, layered fields] [U:1:123] ' +
    'RecvMsgClientLogOnResponse() : processing complete\n';
  const newSession = '[2026-09-08 00:06:28] Client version: 123\n';
  const connected = loggedOn('00:07:00');

  assert.equal(steamConnectedSinceOf('', started, ACCOUNT_ID), false);
  assert.equal(steamConnectedSinceOf(denied, started, ACCOUNT_ID), false);
  assert.equal(steamConnectedSinceOf(oldSession, started, ACCOUNT_ID), true);
  assert.equal(steamConnectedSinceOf(oldSession + denied, started, ACCOUNT_ID), false);
  assert.equal(steamConnectedSinceOf(oldSession + newSession, started, ACCOUNT_ID), false);
  assert.equal(
    steamConnectedSinceOf(oldSession + newSession + connected, started, ACCOUNT_ID),
    true,
  );
  assert.equal(
    steamConnectedSinceOf(
      oldSession + newSession + connected + 'Log session ended\n',
      started,
      ACCOUNT_ID,
    ),
    false,
  );
});

test('a logon another account completed is not this account being connected', () => {
  const started = new Date(2026, 8, 8, 0, 0, 0).getTime();

  assert.equal(steamConnectedSinceOf(loggedOn('00:01:00', '2'), started, ACCOUNT_ID), false);
  assert.equal(steamConnectedSinceOf(loggedOn('00:01:00', '2'), started, '2'), true);
});

test('disconnecting invalidates a logon before Steam updates its state prefix', () => {
  const started = new Date(2026, 8, 8, 0, 0, 0).getTime();

  for (const disconnect of [
    'RecvMsgClientLoggedOff(Service Unavailable)',
    'AsyncDisconnect()',
    'ConnectionDisconnected()',
    'LogOff()',
  ]) {
    assert.equal(
      steamConnectedSinceOf(`${loggedOn('00:01:00')}${disconnect}\n`, started, ACCOUNT_ID),
      false,
    );
  }
});

test('a Steam process needs a successful connection no older than that process', () => {
  const previous = loggedOn('00:01:00');
  const current = loggedOn('00:04:00');
  const startedAt = new Date(2026, 8, 8, 0, 3, 0, 750).getTime();

  assert.equal(steamConnectedSinceOf(previous, startedAt, ACCOUNT_ID), false);
  assert.equal(steamConnectedSinceOf(previous + current, startedAt, ACCOUNT_ID), true);
  assert.equal(
    steamConnectedSinceOf(current, new Date(2026, 8, 8, 0, 4, 0, 999).getTime(), ACCOUNT_ID),
    true,
  );
  assert.equal(
    steamConnectedSinceOf(current, new Date(2026, 8, 8, 0, 4, 1).getTime(), ACCOUNT_ID),
    false,
  );
});

test('a connected session survives rotation when the previous log is read first', () => {
  const previous = loggedOn('00:04:00');
  const current = '[2026-09-08 00:05:00] [Logged On, layered fields] ordinary traffic\n';

  assert.equal(
    steamConnectedSinceOf(previous + current, new Date(2026, 8, 8, 0, 3, 0).getTime(), ACCOUNT_ID),
    true,
  );
});

/** The line Steam writes when a logon of its own completes, as this account unless told otherwise. */
function loggedOn(at: string, account: string = ACCOUNT_ID): string {
  return (
    `[2026-09-08 ${at}] [Logged On, layered fields] [U:1:${account}] ` +
    'RecvMsgClientLogOnResponse() : processing complete\n'
  );
}
