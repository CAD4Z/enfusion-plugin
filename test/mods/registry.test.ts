import assert from 'node:assert/strict';
import { test } from 'node:test';
import { registryValue } from '../../src/mods/registry';

test('reads what reg printed, whatever case the value was written in and spaces and all', () => {
  const output = `
HKEY_LOCAL_MACHINE\\SOFTWARE\\WOW6432Node\\Bohemia Interactive\\DayZ
    MAIN    REG_SZ    D:\\SteamLibrary\\steamapps\\common\\DayZ

`;

  assert.equal(registryValue(output, 'main'), 'D:\\SteamLibrary\\steamapps\\common\\DayZ');
});

test('reads what reg printed with the line endings Windows gives it', () => {
  const output =
    '\r\nHKEY_CURRENT_USER\\SOFTWARE\\Valve\\Steam\r\n    SteamPath    REG_SZ    c:/program files (x86)/steam\r\n\r\n';

  assert.equal(registryValue(output, 'SteamPath'), 'c:/program files (x86)/steam');
});

test('a key that is not there is an empty value, not something to fail over', () => {
  assert.equal(registryValue('', 'main'), '');
  assert.equal(
    registryValue('ERROR: The system was unable to find the specified registry key', 'main'),
    '',
  );
});

test('the value asked for is the one read, not whichever came first', () => {
  const output = `
HKEY_CURRENT_USER\\SOFTWARE\\Bohemia Interactive\\DayZ Tools
    path    REG_SZ    D:\\SteamLibrary\\steamapps\\common\\DayZ Tools
    Exe    REG_SZ    D:\\SteamLibrary\\steamapps\\common\\DayZ Tools\\bin\\launcher
    version    REG_SZ    1.00
`;

  assert.equal(registryValue(output, 'path'), 'D:\\SteamLibrary\\steamapps\\common\\DayZ Tools');
});

/** `reg` speaks the machine's language, so nothing here may depend on the words it uses. */
test('an error in a language nobody parsed still means nothing was found', () => {
  // "ERROR: The system was unable to find the specified registry key or value." in Russian.
  const output =
    '\u041e\u0428\u0418\u0411\u041a\u0410: \u041d\u0435 ' +
    '\u0443\u0434\u0430\u0435\u0442\u0441\u044f \u043d\u0430\u0439\u0442\u0438 ' +
    '\u0443\u043a\u0430\u0437\u0430\u043d\u043d\u044b\u0439 \u0440\u0430\u0437\u0434\u0435\u043b ' +
    '\u0438\u043b\u0438 \u043f\u0430\u0440\u0430\u043c\u0435\u0442\u0440 ' +
    '\u0440\u0435\u0435\u0441\u0442\u0440\u0430.';

  assert.equal(registryValue(output, 'main'), '');
});
