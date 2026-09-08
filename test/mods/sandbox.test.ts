import assert from 'node:assert/strict';
import { test } from 'node:test';
import type { SecondClient } from '../../src/mods/machine';
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
  loginUsersPathOf,
  previousConnectionLogPathOf,
  SIGN_IN_PATIENCE,
  sandboxPlanOf,
  steamAccountIdOf,
  steamConnectedSinceOf,
  steamCommandOf,
  steamExecutableOf,
} from '../../src/mods/sandbox';

/** The Steam3 account id of the account the fixtures below are signed in as. */
const ACCOUNT_ID = '1547129925';

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
  const said =
    'ИНФО: нет запущенных задач, соответствующих указанным критериям.\r\n' +
    '"steam.exe","7488","Console","1","108 192 КБ"\r\n' +
    '"steam.exe","28652","Console","1","144 320 КБ"\r\n';

  assert.deepEqual(imagePidsOf(said), [7488, 28652]);
  assert.deepEqual(imagePidsOf('ИНФО: нет запущенных задач.\r\n'), []);
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
\t"76561199507395653"
\t{
\t\t"AccountName"\t\t"estrv05733"
\t\t"AutoLogin"\t\t"1"
\t\t"MostRecent"\t\t"1"
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
\t\t"MostRecent" "1"
\t}
\t"76561197960265730"
\t{
\t\t"AccountName" "second"
\t\t"MostRecent" "0"
\t}
}`;

  assert.equal(steamAccountIdOf(vdf, 'first'), '1');
  assert.equal(steamAccountIdOf(vdf, 'second'), '2');
});

test('Steam is connected only after its latest session finishes logging on', () => {
  const started = new Date(2026, 8, 8, 0, 0, 0).getTime();
  const oldSession = loggedOn('00:01:00');
  const denied =
    '[2026-09-08 00:02:00] [Logging On, layered fields] [U:1:1547129925] ' +
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
