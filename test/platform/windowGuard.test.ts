import assert from 'node:assert/strict';
import { execFile } from 'node:child_process';
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { test } from 'node:test';
import { promisify } from 'node:util';
import { GUARD_SOURCE, encoded, guardSourceFile, scriptOf } from '../../src/platform/windowGuard';

const run = promisify(execFile);

/** What Windows takes on one command line, less the rest of the guard's own arguments. */
const COMMAND_LINE = 32_767 - 200;

const SOURCE = 'C:\\Users\\dev\\AppData\\Local\\Temp\\enfusion-window-guard-0123456789abcdef.cs';

test('the guard fits on one command line with room to spare, however long the names', () => {
  const command = encoded(scriptOf(`${'x'.repeat(250)}.exe`, Date.now(), `C:\\${'y'.repeat(250)}.cs`));

  assert.ok(command.length < COMMAND_LINE / 4, `${command.length} characters`);
});

test('the encoded command is the script, as PowerShell decodes it', () => {
  const script = scriptOf('DayZDiag_x64.exe', 1_700_000_000_000, SOURCE);

  assert.equal(Buffer.from(encoded(script), 'base64').toString('utf16le'), script);
  assert.match(script, /^Add-Type -Path 'C:\\Users\\dev\\AppData\\Local\\Temp\\enfusion-window-guard-0123456789abcdef\.cs'$/m);
  assert.match(script, /\[EnfusionWindowGuard\]::Run\('DayZDiag_x64\.exe', 1700000000000, /);
});

/** A quotation mark in a name ends PowerShell's string only where it is not doubled. */
test('a quotation mark in a name is doubled rather than ending the string', () => {
  const script = scriptOf("Day'Z.exe", 0, "C:\\O'Brien\\guard.cs");

  assert.match(script, /::Run\('Day''Z\.exe', 0, /);
  assert.match(script, /^Add-Type -Path 'C:\\O''Brien\\guard\.cs'$/m);
});

/**
 * The file is named after what it holds, so a stale one of an older version is never compiled, and
 * one somebody changed or truncated is written again rather than trusted.
 */
test('the C# half is written where it is missing or wrong, and left where it is right', () => {
  const folder = mkdtempSync(join(tmpdir(), 'enfusion-guard-test-'));
  try {
    const path = guardSourceFile(folder);
    assert.equal(readFileSync(path, 'utf8'), GUARD_SOURCE);
    assert.equal(guardSourceFile(folder), path);

    writeFileSync(path, 'truncated');
    guardSourceFile(folder);
    assert.equal(readFileSync(path, 'utf8'), GUARD_SOURCE);
  } finally {
    rmSync(folder, { recursive: true, force: true });
  }
});

/**
 * The C# half is compiled on the developer's machine at every launch, so a mistake in it would
 * only show as a guard that silently never runs. Compiled here instead, the way the guard compiles
 * it, without being run.
 */
test(
  'the Win32 half of the guard compiles',
  { skip: process.platform !== 'win32' && 'PowerShell and Add-Type are Windows only' },
  async () => {
    const folder = mkdtempSync(join(tmpdir(), 'enfusion-guard-test-'));
    try {
      const script = [
        "$ErrorActionPreference = 'Stop'",
        `Add-Type -Path '${guardSourceFile(folder).replace(/'/g, "''")}'`,
        "[Console]::Out.Write([EnfusionWindowGuard].GetMethod('Run').GetParameters().Count)",
      ].join('\n');

      const { stdout } = await run(
        'powershell.exe',
        ['-NoProfile', '-NonInteractive', '-EncodedCommand', encoded(script)],
        { windowsHide: true },
      );

      assert.equal(stdout.trim(), '5');
    } finally {
      rmSync(folder, { recursive: true, force: true });
    }
  },
);
