import assert from 'node:assert/strict';
import { test } from 'node:test';
import { modNameOf } from '../../src/mods/modName';
import {
  type LinkFact,
  type Prefix,
  driveLetterOf,
  currentWorkDriveAction,
  isUnlinked,
  linkPathOf,
  linksOf,
  linksToMake,
  refusalOf,
  workDriveOf,
} from '../../src/mods/workDrive';

const SOURCE = 'F:\\DayZ\\Workdrive';
const ELSEWHERE = 'C:\\Someone\\Else';

const MOUNTED = workDriveOf('P:', SOURCE, SOURCE);
const UNMOUNTED = workDriveOf('P:', SOURCE, '');
const MISMOUNTED = workDriveOf('P:', SOURCE, ELSEWHERE);
const UNSET = workDriveOf('P:', '', '');

const MOD_A: Prefix = {
  prefixRoot: '/f:/Code/Repo/ModA/ModA',
  name: 'ModA',
  modName: modNameOf('ModA'),
  target: 'f:\\Code\\Repo\\ModA\\ModA',
};
const MOD_B: Prefix = {
  prefixRoot: '/f:/Code/Repo/ModB/ModB',
  name: 'ModB',
  modName: modNameOf('ModB'),
  target: 'f:\\Code\\Repo\\ModB\\ModB',
};
const PREFIXES: readonly Prefix[] = [MOD_A, MOD_B];

test('a letter is typed every way there is, and taken the one way the tools take it', () => {
  assert.equal(driveLetterOf('P'), 'P:');
  assert.equal(driveLetterOf('p:'), 'P:');
  assert.equal(driveLetterOf('P:\\'), 'P:');
  assert.equal(driveLetterOf(' p:/ '), 'P:');
});

test('what is not a drive letter at all leaves the drive where most machines have it', () => {
  assert.equal(driveLetterOf(''), 'P:');
  assert.equal(driveLetterOf('Workdrive'), 'P:');
  assert.equal(driveLetterOf('12'), 'P:');
});

test('the drive is mounted, not mounted, set nowhere, or mounted somewhere else', () => {
  assert.equal(MOUNTED.state, 'mounted');
  assert.equal(UNMOUNTED.state, 'unmounted');
  assert.equal(UNSET.state, 'unset');
  assert.equal(MISMOUNTED.state, 'elsewhere');
});

test('one drive button mounts a free letter and unmounts any occupied one', () => {
  assert.equal(currentWorkDriveAction(UNMOUNTED), 'mount');
  assert.equal(currentWorkDriveAction(UNSET), 'mount');
  assert.equal(currentWorkDriveAction(MOUNTED), 'unmount');
  assert.equal(currentWorkDriveAction(MISMOUNTED), 'unmount');
});

test('a folder is the same folder however it was typed, which on Windows is any way at all', () => {
  assert.equal(workDriveOf('P:', SOURCE, 'f:/dayz/workdrive\\').state, 'mounted');
});

/** The whole point of the warning: neither folder is left for the developer to guess at. */
test('a drive mounted somewhere else names both folders, the one it is at and the one it is set to', () => {
  const refusal = refusalOf(MISMOUNTED, 'link') ?? '';

  assert.ok(refusal.includes(ELSEWHERE), refusal);
  assert.ok(refusal.includes(SOURCE), refusal);
});

test('the drive as it should be refuses nothing but a second mount', () => {
  assert.equal(refusalOf(MOUNTED, 'link'), undefined);
  assert.equal(refusalOf(MOUNTED, 'unmount'), undefined);
  assert.notEqual(refusalOf(MOUNTED, 'mount'), undefined);
});

test('an unmounted drive is mounted, and nothing else', () => {
  assert.equal(refusalOf(UNMOUNTED, 'mount'), undefined);
  assert.notEqual(refusalOf(UNMOUNTED, 'unmount'), undefined);
  assert.notEqual(refusalOf(UNMOUNTED, 'link'), undefined);
});

test('with no folder set there is nothing to mount from, and nothing to link into', () => {
  assert.notEqual(refusalOf(UNSET, 'mount'), undefined);
  assert.notEqual(refusalOf(UNSET, 'link'), undefined);
});

/** Freeing a letter is worth doing even when nobody said what the letter was meant to hold. */
test('a letter that is up comes down whatever the settings say about it', () => {
  assert.equal(refusalOf(workDriveOf('P:', '', ELSEWHERE), 'unmount'), undefined);
  assert.equal(refusalOf(MISMOUNTED, 'unmount'), undefined);
});

test('a mod is linked under its prefix root name, in the root of the drive', () => {
  const name = modNameOf('ModA');
  assert.ok(name !== undefined);
  assert.equal(linkPathOf('P:', name), 'P:\\ModA');
});

test('an invalid name has no work-drive path and can never become link work', () => {
  const unsafe: Prefix = {
    prefixRoot: '/f:/Code/Repo/Safe/Victim',
    name: '../Victim',
    modName: undefined,
    target: 'F:\\Victim',
  };
  const links = linksOf([unsafe, MOD_A], MOUNTED, facts([]));

  assert.equal(links[0]?.state, 'invalid');
  assert.equal(links[0]?.path, '');
  assert.match(links[0]?.problem ?? '', /letters, digits and underscores/);
  assert.deepEqual(linksToMake(links).map(name), ['ModA']);
});

test('a checked name borrowed from another mod is not used for a work-drive path', () => {
  const mismatched: Prefix = { ...MOD_A, name: 'ModB' };
  const [link] = linksOf([mismatched], MOUNTED, facts([]));

  assert.equal(link?.state, 'invalid');
  assert.equal(link?.path, '');
  assert.match(link?.problem ?? '', /no longer matches/);
  assert.deepEqual(linksToMake(link === undefined ? [] : [link]), []);
});

test('nothing at the link is a mod waiting to be linked', () => {
  const links = linksOf(PREFIXES, MOUNTED, facts([]));

  assert.deepEqual(
    links.map((link) => [link.path, link.state, link.at]),
    [
      ['P:\\ModA', 'unlinked', ''],
      ['P:\\ModB', 'unlinked', ''],
    ],
  );
});

/** What SetupWorkdrive.bat skipped is what a second run mostly finds, so it cannot be a failure. */
test('a junction already pointing where it should is not an error and not work', () => {
  const links = linksOf(PREFIXES, MOUNTED, facts([['P:\\ModA', pointsAt(MOD_A.target)]]));

  assert.deepEqual(links.map(state), ['linked', 'unlinked']);
  assert.deepEqual(linksToMake(links).map(name), ['ModB']);
});

test('a junction pointing somewhere else is repointed rather than left to build the wrong sources', () => {
  const links = linksOf(PREFIXES, MOUNTED, facts([['P:\\ModA', pointsAt('D:\\Old\\ModA')]]));

  assert.deepEqual(links.map(state), ['elsewhere', 'unlinked']);
  assert.deepEqual(
    links.map((link) => link.at),
    ['D:\\Old\\ModA', ''],
  );
  assert.deepEqual(linksToMake(links).map(name), ['ModA', 'ModB']);
});

test('a junction is the same junction however Windows spelled it back', () => {
  const spelled = pointsAt('F:/Code/REPO/ModA/ModA');
  const links = linksOf(PREFIXES, MOUNTED, facts([['P:\\ModA', spelled]]));

  assert.deepEqual(links.map(state), ['linked', 'unlinked']);
});

/** A real folder on the drive is somebody's data, and unpicking it is not the button's business. */
test('a real folder in the way is shown rather than removed', () => {
  const links = linksOf(PREFIXES, MOUNTED, facts([['P:\\ModA', { kind: 'occupied' }]]));

  assert.deepEqual(links.map(state), ['occupied', 'unlinked']);
  assert.deepEqual(links.filter(isUnlinked).map(name), ['ModA', 'ModB']);
  assert.deepEqual(linksToMake(links).map(name), ['ModB']);
});

test('with the drive down there is nothing to say about any link on it', () => {
  const links = linksOf(PREFIXES, UNMOUNTED, facts([]));

  assert.deepEqual(links.map(state), ['unavailable', 'unavailable']);
  assert.deepEqual(links.filter(isUnlinked), []);
  assert.deepEqual(linksToMake(links), []);
});

/** The link is a fact about `P:\<Name>`, and where the drive is mounted is a fact about `P:`. */
test('a drive mounted elsewhere is still asked what is on it', () => {
  const links = linksOf(PREFIXES, MISMOUNTED, facts([['P:\\ModA', pointsAt(MOD_A.target)]]));

  assert.deepEqual(links.map(state), ['linked', 'unlinked']);
});

/**
 * Two mods of one name want the one `P:\<Name>`. Making both would either stop the run on the
 * second or leave the two of them overwriting each other every time the button is pressed.
 */
test('two mods of the same name are not both made, and the same one wins every run', () => {
  const twin: Prefix = {
    ...MOD_B,
    prefixRoot: '/f:/Other/ModA/ModA',
    name: 'ModA',
    modName: modNameOf('ModA'),
  };
  const both = [MOD_A, twin];

  const fresh = linksOf(both, MOUNTED, facts([]));
  assert.deepEqual(linksToMake(fresh).map(target), [MOD_A.target]);

  // And once the first has it, the second showing "elsewhere" does not take it back off it.
  const after = linksOf(both, MOUNTED, facts([['P:\\ModA', pointsAt(MOD_A.target)]]));
  assert.deepEqual(after.map(state), ['linked', 'elsewhere']);
  assert.deepEqual(linksToMake(after), []);
});

test('a mod keeps hold of the link that is its own, so the panel can find it again', () => {
  const links = linksOf(PREFIXES, MOUNTED, facts([]));

  assert.deepEqual(
    links.map((link) => link.prefixRoot),
    [MOD_A.prefixRoot, MOD_B.prefixRoot],
  );
  assert.deepEqual(
    links.map((link) => link.target),
    [MOD_A.target, MOD_B.target],
  );
});

function facts(entries: readonly (readonly [string, LinkFact])[]): Map<string, LinkFact> {
  return new Map(entries);
}

function pointsAt(target: string): LinkFact {
  return { kind: 'link', target };
}

function state(link: { readonly state: string }): string {
  return link.state;
}

function name(link: { readonly name: string }): string {
  return link.name;
}

function target(link: { readonly target: string }): string {
  return link.target;
}
