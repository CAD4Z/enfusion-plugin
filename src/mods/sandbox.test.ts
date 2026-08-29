import assert from 'node:assert/strict';
import { test } from 'node:test';
import type { SecondClient } from './machine';
import {
  BOX,
  BOX_SETTINGS,
  boxExistsOf,
  boxPidsOf,
  boxPrefixOf,
  boxRootOf,
  gamePrefixOf,
  imagePidsOf,
  loginUsersPathOf,
  SIGN_IN_GRACE,
  sandboxPlanOf,
  signedInNowOf,
  signedInOf,
  steamCommandOf,
  steamExecutableOf,
} from './sandbox';

const SECOND: SecondClient = {
  account: 'estrv05733',
  sandboxie: 'C:\\Program Files\\Sandboxie-Plus',
  steam: 'C:\\Program Files (x86)\\Steam',
};

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

test('the game is put in the box and waited for, and Steam only put in it', () => {
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

test('a sign-in is the account being named in what Steam wrote', () => {
  const vdf = `"users"
{
\t"76561199507395653"
\t{
\t\t"AccountName"\t\t"estrv05733"
\t\t"AutoLogin"\t\t"1"
\t}
}`;

  assert.equal(signedInOf(vdf, 'estrv05733'), true);
  assert.equal(signedInOf(vdf, 'ESTRV05733'), true);
  assert.equal(signedInOf(vdf, 'thehurfy'), false);
  assert.equal(signedInOf('', 'estrv05733'), false);
});

test('a box that has never had a Steam in it has signed nobody in', () => {
  assert.equal(signedInNowOf(undefined, 1000, 1000 + SIGN_IN_GRACE * 2), false);
});

test('a sign-in written since Steam was started is this Steam signing in', () => {
  assert.equal(signedInNowOf(1500, 1000, 1600), true);
});

test('a sign-in the box remembered from last time is not yet an answer', () => {
  assert.equal(signedInNowOf(500, 1000, 1600), false);
});

test('a Steam given long enough to sign itself in is taken at its word', () => {
  assert.equal(signedInNowOf(500, 1000, 1000 + SIGN_IN_GRACE), true);
});

test('a Steam that was already up is asked only for the name', () => {
  assert.equal(signedInNowOf(500, 0, 500), true);
});
