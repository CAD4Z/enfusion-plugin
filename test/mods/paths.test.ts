import assert from 'node:assert/strict';
import { test } from 'node:test';
import {
  literalGlobOf,
  resolveWindows,
  samePath,
  windowsFolder,
  windowsName,
  windowsPath,
} from '../../src/mods/paths';

/** Each glob character stands for itself as a one-character wildcard, so the name still matches. */
test('a file name with glob characters in it becomes a pattern that still matches it', () => {
  assert.equal(literalGlobOf('ground_co.png'), 'ground_co.png');
  assert.equal(literalGlobOf('wall [old] {2}*?.png'), 'wall ?old? ?2???.png');
});

test('parts are joined with one separator, however many the parts brought', () => {
  assert.equal(windowsPath('P:', 'temp'), 'P:\\temp');
  assert.equal(windowsPath('P:\\', '\\temp\\'), 'P:\\temp');
  assert.equal(windowsPath('F:\\Mods', 'Addons', 'ModA.pbo'), 'F:\\Mods\\Addons\\ModA.pbo');
});

/** A single-addon mod has nothing under its prefix root, and joining it must not leave a stray `\`. */
test('an empty part joins nothing rather than a separator', () => {
  assert.equal(windowsPath('P:\\ModA', ''), 'P:\\ModA');
  assert.equal(windowsPath('', 'ModA'), 'ModA');
  assert.equal(windowsPath('', ''), '');
});

/** A builder names a file from the root of the drive, and it joins on under the letter. */
test('a part counted from the root of a drive joins on without doubling the separator', () => {
  assert.equal(windowsPath('P:', '\\ModA\\config.cpp'), 'P:\\ModA\\config.cpp');
});

/** Only the first part keeps what is in front of it, which is what leaves a UNC path a UNC path. */
test('a share keeps the two separators it is known by', () => {
  assert.equal(windowsPath('\\\\build\\mods', 'Addons'), '\\\\build\\mods\\Addons');
});

test('the name is the last segment, and the folder is everything above it', () => {
  assert.equal(windowsName('C:\\keys\\mykey.biprivatekey'), 'mykey.biprivatekey');
  assert.equal(windowsFolder('C:\\keys\\mykey.biprivatekey'), 'C:\\keys');
  assert.equal(windowsName('P:\\ModA\\'), 'ModA');
  assert.equal(windowsFolder('P:\\ModA\\'), 'P:');
});

/** A path typed into a manifest is typed whichever way, and both ways mean the same folder. */
test('either separator is a separator', () => {
  assert.equal(windowsName('F:/Mods/@ModA'), '@ModA');
  assert.equal(windowsFolder('F:/Mods/@ModA'), 'F:/Mods');
});

test('nothing above the last segment is no folder at all', () => {
  assert.equal(windowsFolder('ModA'), '');
  assert.equal(windowsName(''), '');
});

/**
 * The whole reason a relative path is worth anything here: `mod.enf` is text under git, and a
 * mods directory typed as an absolute path is only ever right on the machine that typed it.
 */
test('a relative path is counted from the file that holds it', () => {
  assert.equal(resolveWindows('F:\\Code\\Repo', 'builds'), 'F:\\Code\\Repo\\builds');
  assert.equal(resolveWindows('F:\\Code\\Repo', '..\\builds'), 'F:\\Code\\Repo\\..\\builds');
  assert.equal(resolveWindows('F:\\Code\\Repo', 'out/mods'), 'F:\\Code\\Repo\\out\\mods');
});

test('a path that is already rooted is left where it was typed', () => {
  assert.equal(resolveWindows('F:\\Code', 'P:\\Mods'), 'P:\\Mods');
  assert.equal(resolveWindows('F:\\Code', 'D:/Mods'), 'D:\\Mods');
  // Rooted on whatever drive the process is on, which is still not ours to join onto.
  assert.equal(resolveWindows('F:\\Code', '\\Mods'), '\\Mods');
  assert.equal(resolveWindows('F:\\Code', '\\\\build\\mods'), '\\\\build\\mods');
});

test('a path nobody typed resolves to nothing, rather than to the folder it was counted from', () => {
  assert.equal(resolveWindows('F:\\Code', ''), '');
  assert.equal(resolveWindows('F:\\Code', '   '), '');
});

test('a comparison sees past case, separators, doubled ones and a trailing one', () => {
  assert.equal(samePath('C:\\Mod\\Art\\Icon.png'), samePath('c:/mod/art/icon.png'));
  assert.equal(samePath('C:\\Mod\\Art\\'), samePath('C:\\Mod\\Art'));
  assert.equal(samePath('C:\\Mod\\\\Art'), samePath('C:\\Mod\\Art'));
  // The leading slash a Uri.path carries in front of a drive letter is not part of the path.
  assert.equal(samePath('/C:/Mod/Art'), samePath('C:\\Mod\\Art'));
});

test('a comparison walks the dot segments Windows would have walked', () => {
  assert.equal(samePath('C:\\Mod\\.\\Art\\Icon.png'), samePath('C:\\Mod\\Art\\Icon.png'));
  assert.equal(samePath('C:\\Mod\\GUI\\..\\Art\\Icon.png'), samePath('C:\\Mod\\Art\\Icon.png'));
  assert.equal(samePath('C:\\Mod\\Art\\..\\..\\Mod\\Art'), samePath('C:\\Mod\\Art'));
});

test('no root walks above itself, which is what keeps a drive and a share whole', () => {
  assert.equal(samePath('C:\\..'), 'c:');
  assert.equal(samePath('C:\\..\\..\\Mod'), 'c:/mod');
  // A share is the root of a UNC path, so neither it nor the server above it is walked away.
  assert.equal(samePath('\\\\build\\share\\..\\..'), '//build/share');
  assert.equal(samePath('\\\\build\\share\\Mods\\..'), '//build/share');
  assert.equal(samePath('\\\\build\\share'), '//build/share');
  assert.equal(samePath('\\\\build\\'), '//build');
});

test('what a UNC root has under it stays under it, rather than running into the share name', () => {
  assert.equal(samePath('\\\\srv\\share\\tex\\a.png'), '//srv/share/tex/a.png');
  assert.equal(samePath('\\\\srv\\share\\tex\\a.png'), samePath('//SRV/Share/./tex/a.png'));
  // Two different files whose names would touch if the separator went missing.
  assert.notEqual(samePath('\\\\srv\\share\\ab.png'), samePath('\\\\srv\\shar\\eab.png'));
  // A relative path has no root to stop at, so the walk it could not take is kept.
  assert.equal(samePath('..\\Mod'), '../mod');
});
