import assert from 'node:assert/strict';
import { test } from 'node:test';
import { resolveWindows, windowsFolder, windowsName, windowsPath } from './paths';

test('parts are joined with one separator, however many the parts brought', () => {
  assert.equal(windowsPath('P:', 'temp'), 'P:\\temp');
  assert.equal(windowsPath('P:\\', '\\temp\\'), 'P:\\temp');
  assert.equal(windowsPath('F:\\Mods', 'Addons', 'CADCore.pbo'), 'F:\\Mods\\Addons\\CADCore.pbo');
});

/** A single-addon mod has nothing under its prefix root, and joining it must not leave a stray `\`. */
test('an empty part joins nothing rather than a separator', () => {
  assert.equal(windowsPath('P:\\CADCore', ''), 'P:\\CADCore');
  assert.equal(windowsPath('', 'CADCore'), 'CADCore');
  assert.equal(windowsPath('', ''), '');
});

/** A builder names a file from the root of the drive, and it joins on under the letter. */
test('a part counted from the root of a drive joins on without doubling the separator', () => {
  assert.equal(windowsPath('P:', '\\CADCore\\config.cpp'), 'P:\\CADCore\\config.cpp');
});

/** Only the first part keeps what is in front of it, which is what leaves a UNC path a UNC path. */
test('a share keeps the two separators it is known by', () => {
  assert.equal(windowsPath('\\\\build\\mods', 'Addons'), '\\\\build\\mods\\Addons');
});

test('the name is the last segment, and the folder is everything above it', () => {
  assert.equal(windowsName('C:\\keys\\hurfy.biprivatekey'), 'hurfy.biprivatekey');
  assert.equal(windowsFolder('C:\\keys\\hurfy.biprivatekey'), 'C:\\keys');
  assert.equal(windowsName('P:\\CADCore\\'), 'CADCore');
  assert.equal(windowsFolder('P:\\CADCore\\'), 'P:');
});

/** A path typed into a manifest is typed whichever way, and both ways mean the same folder. */
test('either separator is a separator', () => {
  assert.equal(windowsName('F:/Mods/@CADCore'), '@CADCore');
  assert.equal(windowsFolder('F:/Mods/@CADCore'), 'F:/Mods');
});

test('nothing above the last segment is no folder at all', () => {
  assert.equal(windowsFolder('CADCore'), '');
  assert.equal(windowsName(''), '');
});

/**
 * The whole reason a relative path is worth anything here: `mod.enf` is text under git, and a
 * mods directory typed as an absolute path is only ever right on the machine that typed it.
 */
test('a relative path is counted from the file that holds it', () => {
  assert.equal(resolveWindows('F:\\Code\\cad4z', 'builds'), 'F:\\Code\\cad4z\\builds');
  assert.equal(resolveWindows('F:\\Code\\cad4z', '..\\builds'), 'F:\\Code\\cad4z\\..\\builds');
  assert.equal(resolveWindows('F:\\Code\\cad4z', 'out/mods'), 'F:\\Code\\cad4z\\out\\mods');
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
