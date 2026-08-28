import assert from 'node:assert/strict';
import { test } from 'node:test';
import {
  STEAM_APP,
  appManifestPath,
  appPath,
  installDirOf,
  libraryFoldersPath,
  libraryOf,
  parseKeyValues,
} from './steam';

/** The file as Steam writes it, cut down to the two libraries and the apps that matter here. */
const LIBRARIES = `"libraryfolders"
{
\t"0"
\t{
\t\t"path"\t\t"C:\\\\Program Files (x86)\\\\Steam"
\t\t"label"\t\t""
\t\t"apps"
\t\t{
\t\t\t"228980"\t\t"480519205"
\t\t}
\t}
\t"1"
\t{
\t\t"path"\t\t"F:\\\\SteamLibrary"
\t\t"label"\t\t""
\t\t"apps"
\t\t{
\t\t\t"221100"\t\t"25563415305"
\t\t\t"830640"\t\t"543457582"
\t\t}
\t}
}
`;

test('the library holding an app is the one whose apps list names it', () => {
  assert.equal(libraryOf(LIBRARIES, STEAM_APP.dayz), 'F:\\SteamLibrary');
  assert.equal(libraryOf(LIBRARIES, STEAM_APP.dayzTools), 'F:\\SteamLibrary');
  assert.equal(libraryOf(LIBRARIES, '228980'), 'C:\\Program Files (x86)\\Steam');
});

test('an app no library holds is nowhere, not the first library', () => {
  assert.equal(libraryOf(LIBRARIES, '107410'), undefined);
});

test('a file that is not there, or is halfway written, leaves the app nowhere', () => {
  assert.equal(libraryOf('', STEAM_APP.dayz), undefined);
  assert.equal(libraryOf('"libraryfolders"\n{\n\t"0"\n\t{\n\t\t"path"', STEAM_APP.dayz), undefined);
});

test('the folder an app was installed under comes from its own manifest', () => {
  const manifest = `"AppState"
{
\t"appid"\t\t"830640"
\t"name"\t\t"DayZ Tools"
\t"installdir"\t\t"DayZ Tools"
\t"StateFlags"\t\t"4"
}
`;

  assert.equal(installDirOf(manifest), 'DayZ Tools');
  assert.equal(installDirOf(''), undefined);
});

test('the paths are built the way Steam lays a library out', () => {
  assert.equal(
    libraryFoldersPath('C:\\Program Files (x86)\\Steam'),
    'C:\\Program Files (x86)\\Steam\\steamapps\\libraryfolders.vdf',
  );
  assert.equal(
    appManifestPath('F:\\SteamLibrary', STEAM_APP.dayz),
    'F:\\SteamLibrary\\steamapps\\appmanifest_221100.acf',
  );
  assert.equal(
    appPath('F:\\SteamLibrary', 'DayZ Tools'),
    'F:\\SteamLibrary\\steamapps\\common\\DayZ Tools',
  );
});

test('a backslash in a quoted string stands for the character after it', () => {
  const values = parseKeyValues('"a" "C:\\\\Steam" "b" "say \\"hi\\""');

  assert.equal(values.a, 'C:\\Steam');
  assert.equal(values.b, 'say "hi"');
});

test('comments are not values, and unquoted tokens are read as tokens', () => {
  const values = parseKeyValues(`
// the whole line is a comment
"outer"
{
  inner 12
}
`);

  assert.deepEqual(values, { outer: { inner: '12' } });
});
