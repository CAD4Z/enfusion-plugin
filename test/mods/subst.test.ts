import assert from 'node:assert/strict';
import { test } from 'node:test';
import {
  mountArguments,
  mountCommandOf,
  mountedAt,
  unmountArguments,
  unmountCommandOf,
} from '../../src/mods/subst';

/** What `subst` prints with no arguments, on a machine with two letters up. */
const MOUNTED = ['P:\\: => F:\\DayZ\\Workdrive', 'X:\\: => C:\\Temp\\Scratch', ''].join('\r\n');

test('the folder a letter is mounted from is read off what subst printed', () => {
  assert.equal(mountedAt(MOUNTED, 'P:'), 'F:\\DayZ\\Workdrive');
  assert.equal(mountedAt(MOUNTED, 'X:'), 'C:\\Temp\\Scratch');
});

test('a letter subst did not print is a letter that is mounted nowhere', () => {
  assert.equal(mountedAt(MOUNTED, 'Z:'), '');
  assert.equal(mountedAt('', 'P:'), '');
});

test('a letter is the same letter however it was cased', () => {
  assert.equal(mountedAt('p:\\: => F:\\DayZ\\Workdrive', 'P:'), 'F:\\DayZ\\Workdrive');
  assert.equal(mountedAt(MOUNTED, 'p:'), 'F:\\DayZ\\Workdrive');
});

/** The output is paths and an arrow; whatever `subst` says in words says nothing to us. */
test('a line that is not a mapping is not read as one', () => {
  assert.equal(mountedAt('Invalid parameter - P:', 'P:'), '');
  assert.equal(mountedAt('Неверный параметр - P:', 'P:'), '');
});

test('a folder with spaces in it survives being read back', () => {
  assert.equal(
    mountedAt('P:\\: => C:\\Program Files\\DayZ Tools', 'P:'),
    'C:\\Program Files\\DayZ Tools',
  );
});

test('the syntax nobody should have to remember is written down once', () => {
  assert.deepEqual(mountArguments('P:', 'F:\\DayZ\\Workdrive'), ['P:', 'F:\\DayZ\\Workdrive']);
  assert.deepEqual(unmountArguments('P:'), ['P:', '/D']);
});

/**
 * DayZ Tools deliberately repeats the mapping in an elevated child. A bare `subst` from VS Code
 * leaves an elevated Workbench unable to see P:, so the shipped helper is the P: implementation
 * whenever it is available.
 */
test('the P drive is mounted through DayZ WorkDrive for both Windows token contexts', () => {
  const tool = 'F:\\DayZ Tools\\Bin\\WorkDrive\\WorkDrive.exe';

  assert.deepEqual(mountCommandOf('P:', 'F:\\DayZ\\Workdrive', tool), {
    file: tool,
    args: ['/y', '/Silent', '/nowarnings', '/mount', 'P:', 'F:\\DayZ\\Workdrive'],
  });
  assert.deepEqual(unmountCommandOf('P:', tool), {
    file: tool,
    args: ['/y', '/Silent', '/nowarnings', '/unmount', 'P:'],
  });
});

test('subst remains the fallback without DayZ WorkDrive and for a nonstandard letter', () => {
  const tool = 'F:\\DayZ Tools\\Bin\\WorkDrive\\WorkDrive.exe';

  assert.deepEqual(mountCommandOf('P:', 'F:\\DayZ\\Workdrive', ''), {
    file: 'subst',
    args: ['P:', 'F:\\DayZ\\Workdrive'],
  });
  assert.deepEqual(unmountCommandOf('X:', tool), {
    file: 'subst',
    args: ['X:', '/D'],
  });
});
