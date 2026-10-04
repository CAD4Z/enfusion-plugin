import assert from 'node:assert/strict';
import { execFile } from 'node:child_process';
import { mkdtemp, rm, stat, utimes, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { test } from 'node:test';
import { promisify } from 'node:util';

// CI runs this seam after building native code; the plain-Node job does not require a C compiler.
const executable = process.env.EDDS_TEST_CONVERTER;
const run = promisify(execFile);

/**
 * VS Code reads a file time through a millisecond `Date`, which rounds; the converter truncates.
 * A file written 0.7 ms into a millisecond is the case where the two disagree by one.
 */
test('a revision a millisecond Date rounded still names the same file', { skip: executable === undefined }, async (context) => {
  assert.ok(executable);
  const folder = await mkdtemp(path.join(tmpdir(), 'enfusion-revision-'));
  context.after(async () => {
    assert.equal(path.dirname(path.resolve(folder)), path.resolve(tmpdir()));
    await rm(folder, { recursive: true, force: true });
  });
  const source = path.join(folder, 'source.tga');
  const image = Buffer.alloc(18 + 16);
  image[2] = 2;
  image.writeUInt16LE(2, 12);
  image.writeUInt16LE(2, 14);
  image[16] = 32;
  image[17] = 40;
  image.fill(255, 18);
  await writeFile(source, image);
  await utimes(source, 1_700_000_000, 1_700_000_000.0007);
  const { size, mtimeMs } = await stat(source);
  assert.notEqual(Math.round(mtimeMs), Math.trunc(mtimeMs));

  const convert = (modified: number, output: string) => run(executable, [
    'edds', 'convert', '--machine', '--protocol', '1', '--input', source,
    '--output', path.join(folder, output),
    '--expect-source-revision', `${size}:${modified}`,
    '--expect-output-revision', 'missing', '--expect-metadata-revision', 'missing',
  ], { windowsHide: true });

  await convert(Math.round(mtimeMs), 'rounded.edds');
  await convert(Math.trunc(mtimeMs), 'truncated.edds');
  await assert.rejects(convert(Math.trunc(mtimeMs) + 2, 'later.edds'), (error: { code?: unknown; stdout?: unknown }) => {
    assert.equal(error.code, 3);
    assert.match(String(error.stdout), /stale-source/);
    return true;
  });
});
