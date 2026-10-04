import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { existsSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { test } from 'node:test';
import { inspectionOf, previewOf } from '../../../src/mods/texture/edds';
import { analysisOf, pixelAt } from '../../../src/mods/texture/textureAnalysis';
import { DEFAULT_TEXTURE_PROFILE } from '../../../src/mods/texture/textureConversion';

const executable = path.resolve('dist/native/win32-x64/enfusion.exe');

test('bundled converter pixels drive histogram, sampling, GPU memory and RMSE without writing artifacts', {
  skip: process.platform !== 'win32' || !existsSync(executable) ? 'Requires the staged Windows executable.' : false,
}, () => {
  const folder = mkdtempSync(path.join(tmpdir(), 'enfusion-analysis-'));
  try {
    const input = path.join(folder, 'owned.tga');
    const output = path.join(folder, 'owned.edds');
    // Two top-left RGBA texels: (10,20,30,40) and (50,60,70,80).
    writeFileSync(input, Uint8Array.from([
      0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 1, 0, 32, 0x28,
      30, 20, 10, 40, 70, 60, 50, 80,
    ]));
    const run = (...args: string[]): string => execFileSync(executable, ['edds', ...args, '--machine', '--protocol', '1'], {
      encoding: 'utf8', windowsHide: true, shell: false,
    });
    run('convert', '--input', input, '--output', output, '--generate-mips', 'true', '--format-compress', 'copy');
    const inspection = inspectionOf(run('inspect', '--input', output));
    const result = previewOf(run('preview', '--input', output, '--mip', '0'));
    const source = { level: 0, width: 2, height: 1, rgba: Uint8Array.from([10, 20, 30, 40, 50, 60, 70, 80]) };
    const before = [readFileSync(input), readFileSync(output)];
    const analysis = analysisOf({ inspection, source, result, profile: DEFAULT_TEXTURE_PROFILE }, { side: 'result', channel: 'rgba' });
    assert.equal(analysis.histogram.kind, 'available');
    if (analysis.histogram.kind !== 'available') return;
    assert.equal(analysis.histogram.value.series[0]?.bins[10], 1);
    assert.equal(analysis.histogram.value.series[3]?.bins[80], 1);
    assert.deepEqual(analysis.memory, { kind: 'available', value: { mipBytes: 8, chainBytes: 12 } });
    assert.equal(analysis.error.kind === 'available' && analysis.error.value.rmse, 0);
    assert.deepEqual(pixelAt({ ...result, channels: inspection.channels }, { x: 1.5, y: 0.5 }, { left: 0, top: 0, width: 2, height: 1 }), {
      kind: 'available', value: { x: 1, y: 0, level: 0, channels: 'RGBA', values: [50, 60, 70, 80] },
    });
    const mip = previewOf(run('preview', '--input', output, '--mip', '1'));
    assert.deepEqual([...mip.rgba], [30, 40, 50, 60]);
    const small = analysisOf({ inspection, result: mip }, { side: 'result', channel: 'green' });
    assert.equal(small.histogram.kind === 'available' && small.histogram.value.series[0]?.bins[40], 1);
    assert.deepEqual([readFileSync(input), readFileSync(output)], before);
    assert.equal(existsSync(`${output}.meta`), false);
  } finally {
    assert.equal(path.dirname(folder), path.resolve(tmpdir()));
    rmSync(folder, { recursive: true, force: true });
  }
});
