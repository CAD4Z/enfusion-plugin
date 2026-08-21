import assert from 'node:assert/strict';
import { test } from 'node:test';
import {
  type MachineSettings,
  builderOf,
  environmentOf,
  environmentPaths,
  isWanting,
} from './machine';

const SETTINGS: MachineSettings = {
  dayz: 'F:\\SteamLibrary\\steamapps\\common\\DayZ',
  dayzTools: 'F:\\SteamLibrary\\steamapps\\common\\DayZ Tools',
  privateKey: 'F:\\Keys\\CAD4Z.biprivatekey',
  workDrive: 'F:\\DayZ\\Workdrive',
  filePatchingRoot: '',
  builder: 'pboProject',
};

test('everything the machine was asked for is there', () => {
  const environment = environmentOf(SETTINGS, [
    SETTINGS.dayz,
    SETTINGS.dayzTools,
    SETTINGS.privateKey,
    SETTINGS.workDrive,
  ]);

  assert.deepEqual(environment, [
    {
      kind: 'dayz',
      setting: 'enfusion.dayz.path',
      path: SETTINGS.dayz,
      state: 'ok',
      optional: false,
    },
    {
      kind: 'dayzTools',
      setting: 'enfusion.dayzTools.path',
      path: SETTINGS.dayzTools,
      state: 'ok',
      optional: false,
    },
    {
      kind: 'privateKey',
      setting: 'enfusion.signing.privateKey',
      path: SETTINGS.privateKey,
      state: 'ok',
      optional: true,
    },
    {
      kind: 'workDrive',
      setting: 'enfusion.workDrive.source',
      path: SETTINGS.workDrive,
      state: 'ok',
      optional: false,
    },
  ]);
});

test('a path that was set but is not there is told apart from one nobody set', () => {
  const environment = environmentOf({ ...SETTINGS, dayzTools: '' }, [SETTINGS.dayz]);

  assert.deepEqual(
    environment.map((entry) => [entry.kind, entry.state]),
    [
      ['dayz', 'ok'],
      ['dayzTools', 'unset'],
      ['privateKey', 'missing'],
      ['workDrive', 'missing'],
    ],
  );
});

test('an unset private key means the pbo goes unsigned, so it is the one thing that is optional', () => {
  const environment = environmentOf({ ...SETTINGS, privateKey: '' }, []);

  assert.deepEqual(
    environment.filter((entry) => entry.optional).map((entry) => [entry.kind, entry.state]),
    [['privateKey', 'unset']],
  );
});

test('what wants attention is a gap, not a choice: an unsigned pbo is nobody in the way', () => {
  const environment = environmentOf({ ...SETTINGS, privateKey: '', dayzTools: '' }, [
    SETTINGS.dayz,
  ]);

  assert.deepEqual(
    environment.filter(isWanting).map((entry) => entry.kind),
    ['dayzTools', 'workDrive'],
  );
});

/** The check on the disk and the report of it are read off one table, so they cannot disagree. */
test('the paths to ask the disk about are the paths the environment is made of', () => {
  const settings = { ...SETTINGS, dayzTools: '' };

  assert.deepEqual(environmentPaths(settings), [
    settings.dayz,
    settings.privateKey,
    settings.workDrive,
  ]);
  assert.deepEqual(
    environmentOf(settings, environmentPaths(settings)).map((entry) => entry.state),
    ['ok', 'unset', 'ok', 'ok'],
  );
});

test('a path is the same path however it was typed, which on Windows is any way at all', () => {
  const environment = environmentOf(
    { ...SETTINGS, dayz: 'F:/steamlibrary/steamapps/common/dayz/' },
    ['F:\\SteamLibrary\\steamapps\\common\\DayZ'],
  );

  assert.equal(environment[0]?.state, 'ok');
});

test('a builder the settings do not name is the one most machines have', () => {
  assert.equal(builderOf('AddonBuilder'), 'AddonBuilder');
  assert.equal(builderOf('pboProject'), 'pboProject');
  assert.equal(builderOf(''), 'pboProject');
});
