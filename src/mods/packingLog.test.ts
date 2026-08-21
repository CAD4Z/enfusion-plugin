import assert from 'node:assert/strict';
import { test } from 'node:test';
import { fileOf, problemsOf } from './packingLog';
import type { Link } from './workDrive';

/**
 * Taken off `P:\temp\EnfusionProbe.packing.log` after pboProject was pointed at a config with a
 * missing semicolon in it. The complaint comes twice: once with the file, and once without, and
 * the second copy has nowhere to send anybody.
 */
const PBOPROJECT = `Processing \\EnfusionProbe...
verifying model.cfgs(if any)...
<scanning files to pack (and verifying mlods if any)>

</end scan>
<MakePbo start>
config.cpp:lint checking...

\\EnfusionProbe\\config.cpp Rapify:circa Line 6 Expected Semicolon OR bad array syntax
circa Line 6 Expected Semicolon OR bad array syntax
pbo_Make failed
EnfusionProbe.pbo not produced due to error(s)`;

/** And this off `AddonBuilder.User.rpt`, for the same addon with an undefined base class in it. */
const ADDON_BUILDER = `2026-08-21 11:59:03,546 [ INFO]   1:  - Converting cfg "P:\\EnfusionProbe\\config.cpp"
2026-08-21 11:59:03,593 [ERROR]   1: CfgConvert returned error. Canceling... [result]=1
2026-08-21 11:59:03,593 [ERROR]   1: !> File P:\\EnfusionProbe\\config.cpp, line 2: /CfgPatches.EnfusionProbe: Undefined base class 'NoSuchBaseAtAll'
2026-08-21 11:59:03,593 [ERROR]   1: !> Config : some input after EndOfFile.
2026-08-21 11:59:03,594 [ERROR]   1: Build failed`;

const CORE: Link = {
  prefixRoot: '/f:/Code/CADCore/CADCore',
  name: 'CADCore',
  path: 'P:\\CADCore',
  target: 'F:\\Code\\CADCore\\CADCore',
  at: 'F:\\Code\\CADCore\\CADCore',
  state: 'linked',
};

test('pboProject names the file from the root of the drive, and the line about where', () => {
  assert.deepEqual(problemsOf(PBOPROJECT), [
    {
      file: '\\EnfusionProbe\\config.cpp',
      line: 6,
      message: 'Expected Semicolon OR bad array syntax',
    },
  ]);
});

test('the config parser names the file in full, whatever the log put in front of the line', () => {
  assert.deepEqual(problemsOf(ADDON_BUILDER), [
    {
      file: 'P:\\EnfusionProbe\\config.cpp',
      line: 2,
      message: "/CfgPatches.EnfusionProbe: Undefined base class 'NoSuchBaseAtAll'",
    },
  ]);
});

/** Both builders say the same thing more than once; a developer wants to be sent there once. */
test('the same complaint about the same line is reported once', () => {
  const twice = `${ADDON_BUILDER}\n${ADDON_BUILDER}`;

  assert.equal(problemsOf(twice).length, 1);
});

test('a build that went through says nothing about any place, and reads as nothing wrong', () => {
  const success = `Processing \\CADCore...
<MakePbo start>
config.cpp:lint checking...
File written to P:\\Mods\\@CADCore\\addons\\CADCore.pbo
<MakePbo end>
success`;

  assert.deepEqual(problemsOf(success), []);
  assert.deepEqual(problemsOf(''), []);
});

test('a place with nothing said about it is not a problem worth sending anybody to', () => {
  assert.deepEqual(problemsOf('\\CADCore\\config.cpp Rapify:circa Line 3 '), []);
});

/** The developer has the file open in the workspace; a second way to the same bytes is a second tab. */
test('a file on the work drive is reported as the file in the workspace it is linked from', () => {
  assert.equal(
    fileOf('\\CADCore\\Scripts\\3_Game\\Thing.c', 'P:', [CORE]),
    'F:\\Code\\CADCore\\CADCore\\Scripts\\3_Game\\Thing.c',
  );
  assert.equal(
    fileOf('P:\\CADCore\\config.cpp', 'P:', [CORE]),
    'F:\\Code\\CADCore\\CADCore\\config.cpp',
  );
  assert.equal(fileOf('p:/cadcore/config.cpp', 'P:', [CORE]), 'F:\\Code\\CADCore\\CADCore\\config.cpp');
});

test('the prefix root itself is the mod itself, not a file below it', () => {
  assert.equal(fileOf('P:\\CADCore', 'P:', [CORE]), 'F:\\Code\\CADCore\\CADCore');
});

/** A vanilla file is on the drive too, and it opens through the drive perfectly well. */
test('a file under no link of ours is left as the builder named it', () => {
  assert.equal(fileOf('\\DZ\\data\\config.cpp', 'P:', [CORE]), 'P:\\DZ\\data\\config.cpp');
  assert.equal(fileOf('C:\\Elsewhere\\config.cpp', 'P:', [CORE]), 'C:\\Elsewhere\\config.cpp');
});

/** A mod whose name is the start of another's is not the other one. */
test('a link is matched by whole folders, never by the letters its name starts with', () => {
  const other: Link = { ...CORE, name: 'CADCoreExtra', path: 'P:\\CADCoreExtra', target: 'F:\\X' };

  assert.equal(fileOf('\\CADCoreExtra\\config.cpp', 'P:', [CORE, other]), 'F:\\X\\config.cpp');
});
