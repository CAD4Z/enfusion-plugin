import assert from 'node:assert/strict';
import { test } from 'node:test';
import { registryValue } from './registry';

test('reads what reg printed, whatever case the value was written in and spaces and all', () => {
  const output = `
HKEY_LOCAL_MACHINE\\SOFTWARE\\WOW6432Node\\Bohemia Interactive\\DayZ
    MAIN    REG_SZ    F:\\SteamLibrary\\steamapps\\common\\DayZ

`;

  assert.equal(registryValue(output, 'main'), 'F:\\SteamLibrary\\steamapps\\common\\DayZ');
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
    path    REG_SZ    F:\\SteamLibrary\\steamapps\\common\\DayZ Tools
    Exe    REG_SZ    F:\\SteamLibrary\\steamapps\\common\\DayZ Tools\\bin\\launcher
    version    REG_SZ    1.00
`;

  assert.equal(registryValue(output, 'path'), 'F:\\SteamLibrary\\steamapps\\common\\DayZ Tools');
});

/** `reg` speaks the machine's language, so nothing here may depend on the words it uses. */
test('an error in a language nobody parsed still means nothing was found', () => {
  const output = 'ОШИБКА: Не удается найти указанный раздел или параметр реестра.';

  assert.equal(registryValue(output, 'main'), '');
});
