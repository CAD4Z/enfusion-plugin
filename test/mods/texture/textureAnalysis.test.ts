import assert from 'node:assert/strict';
import { test } from 'node:test';
import {
  histogramOf, pixelAt, errorMetricOf, runtimeMemoryOf, analysisOf, ANALYSIS_MAX_PIXELS, cachePreview, PREVIEW_CACHE_BYTES,
  type AnalysisPixels,
} from '../../../src/mods/texture/textureAnalysis';
import { DEFAULT_TEXTURE_PROFILE } from '../../../src/mods/texture/textureConversion';
import { inspectionOf } from '../../../src/mods/texture/edds';

const pixels: AnalysisPixels = {
  level: 0, width: 2, height: 1, channels: 'RGBA',
  rgba: Uint8Array.from([10, 20, 30, 40, 50, 60, 70, 80]),
};

test('histograms count actual samples, including invisible RGB under alpha, in the selected channels', () => {
  const histogram = histogramOf(pixels, 'rgba');
  assert.equal(histogram.kind, 'available');
  if (histogram.kind !== 'available') return;
  assert.equal(histogram.value.pixelCount, 2);
  assert.deepEqual(histogram.value.series.map(({ channel, bins }) => [channel, [...bins.entries()].filter(([, n]) => n)]), [
    ['R', [[10, 1], [50, 1]]], ['G', [[20, 1], [60, 1]]],
    ['B', [[30, 1], [70, 1]]], ['A', [[40, 1], [80, 1]]],
  ]);
  const alpha = histogramOf(pixels, 'alpha');
  assert.equal(alpha.kind === 'available' && alpha.value.series.length, 1);
  assert.equal(histogramOf({ ...pixels, channels: 'R' }, 'alpha').kind, 'unavailable');
});

test('pixel inspection maps the displayed canvas rectangle after zoom and pan, without reading composited pixels', () => {
  assert.deepEqual(pixelAt(pixels, { x: 103, y: 51 }, { left: 100, top: 50, width: 4, height: 2 }), {
    kind: 'available', value: { x: 1, y: 0, level: 0, channels: 'RGBA', values: [50, 60, 70, 80] },
  });
  assert.deepEqual(pixelAt(pixels, { x: 0, y: 0 }, { left: -2, top: -1, width: 4, height: 2 }), {
    kind: 'available', value: { x: 1, y: 0, level: 0, channels: 'RGBA', values: [50, 60, 70, 80] },
  });
  for (const x of [99, 104, NaN]) {
    assert.equal(pixelAt(pixels, { x, y: 51 }, { left: 100, top: 50, width: 4, height: 2 }).kind, 'unavailable');
  }
});

test('RMSE uses carried selected channels and deterministic native sample units', () => {
  const result = { ...pixels, rgba: Uint8Array.from([13, 24, 30, 40, 53, 64, 70, 80]) };
  assert.deepEqual(errorMetricOf(pixels, result, 'rgba'), {
    kind: 'available', value: { rmse: 2.5, samples: 8, units: 'byte values (0–255)' },
  });
  const redOnly = errorMetricOf(pixels, result, 'red');
  assert.equal(redOnly.kind === 'available' && redOnly.value.rmse, 3);
  const red = errorMetricOf(pixels, { ...result, channels: 'R' }, 'rgba');
  assert.equal(red.kind === 'available' && red.value.rmse, 3);
  assert.equal(errorMetricOf(pixels, { ...result, level: 1 }, 'rgba').kind, 'unavailable');
});

function inspection() {
  return inspectionOf(JSON.stringify({
    protocolVersion: 1, kind: 'inspect', width: 2, height: 1, mipCount: 2,
    pixelFormat: 'BGRA8', channels: 'RGBA', previewSupported: true,
    dds: { flags: 0, pitchOrLinearSize: 8, depth: 0, pixelFormatFlags: 65, fourCC: 'NONE',
      rgbBitCount: 32, rMask: 16711680, gMask: 65280, bMask: 255, aMask: 4278190080,
      caps: 4096, caps2: 0, dxgiFormat: 0, resourceDimension: 0, arraySize: 0, miscFlag: 0 },
    mips: [
      { level: 0, width: 2, height: 1, container: 'LZ4', storedBytes: 5, decodedBytes: 8 },
      { level: 1, width: 1, height: 1, container: 'COPY', storedBytes: 4, decodedBytes: 4 },
    ],
  }));
}

test('runtime size is the actual GPU payload across the mip chain, never the compressed container size', () => {
  assert.deepEqual(runtimeMemoryOf(inspection(), 1), {
    kind: 'available', value: { mipBytes: 4, chainBytes: 12 },
  });
  const bc = { ...inspection(), pixelFormat: 'DXT1' as const, channels: 'RGB' as const,
    mips: [{ level: 0, width: 2, height: 1, container: 'LZ4' as const, storedBytes: 5, decodedBytes: 8 },
      { level: 1, width: 1, height: 1, container: 'COPY' as const, storedBytes: 8, decodedBytes: 8 }] };
  assert.deepEqual(runtimeMemoryOf(bc, 0), { kind: 'available', value: { mipBytes: 8, chainBytes: 16 } });
});

test('analysis selections are read-only and follow Source, Result, channel and actual mip', () => {
  const source = { ...pixels, rgba: Uint8Array.from(pixels.rgba) };
  const result = { ...source, rgba: Uint8Array.from([13, 24, 30, 40, 53, 64, 70, 80]) };
  const input = { inspection: inspection(), source, result, profile: DEFAULT_TEXTURE_PROFILE };
  const before = structuredClone(input);
  const sourceView = analysisOf(input, { side: 'source', channel: 'red' });
  const resultView = analysisOf(input, { side: 'result', channel: 'green' });
  assert.equal(sourceView.histogram.kind === 'available' && sourceView.histogram.value.series[0]?.bins[10], 1);
  assert.equal(resultView.histogram.kind === 'available' && resultView.histogram.value.series[0]?.bins[24], 1);
  assert.equal(resultView.error.kind === 'available' && resultView.error.value.rmse, 4);
  assert.deepEqual(input, before);
  const smaller = { ...result, level: 1, width: 1, rgba: Uint8Array.from([4, 3, 2, 1]) };
  const mip = analysisOf({ ...input, result: smaller }, { side: 'result', channel: 'alpha' });
  assert.equal(mip.histogram.kind === 'available' && mip.histogram.value.series[0]?.bins[1], 1);
  assert.equal(mip.error.kind, 'unavailable');
});

test('standalone and header-only textures explain unavailable analysis without inventing samples', () => {
  const standalone = analysisOf({ inspection: inspection(), result: { ...pixels, rgba: Uint8Array.from(pixels.rgba) } }, { side: 'result', channel: 'rgba' });
  assert.equal(standalone.histogram.kind, 'available');
  assert.deepEqual(standalone.error, { kind: 'unavailable', reason: 'Standalone EDDS has no Source pipeline pixels to compare.' });
  const unsupported = analysisOf({ inspection: { ...inspection(), channels: 'UNKNOWN',
    pixels: { kind: 'unsupported', reason: 'Unsupported DXGI_200 decoder.' } } }, { side: 'result', channel: 'rgba' });
  assert.deepEqual(unsupported.histogram, { kind: 'unavailable', reason: 'Unsupported DXGI_200 decoder.' });
  assert.equal(unsupported.memory.kind, 'unavailable');
});

test('oversized and malformed samples are refused before walking pixels', () => {
  for (const candidate of [
    { ...pixels, width: ANALYSIS_MAX_PIXELS + 1 },
    { ...pixels, rgba: new Uint8Array() },
    { ...pixels, width: NaN },
  ]) {
    assert.equal(histogramOf(candidate, 'rgba').kind, 'unavailable');
    assert.equal(pixelAt(candidate, { x: 0, y: 0 }, { left: 0, top: 0, width: 2, height: 1 }).kind, 'unavailable');
    assert.equal(errorMetricOf(candidate, pixels, 'rgba').kind, 'unavailable');
  }
});

test('float decoder samples retain negative values and dynamic range in the same analysis contract', () => {
  const hdr: AnalysisPixels = { level: 0, width: 1, height: 1, channels: 'RGB', rgba: new Float32Array([-2, 4, 8, 1]) };
  const histogram = histogramOf(hdr, 'rgba');
  assert.equal(histogram.kind, 'available');
  if (histogram.kind !== 'available') return;
  assert.equal(histogram.value.minimum, -2);
  assert.equal(histogram.value.maximum, 8);
  assert.equal(histogram.value.series[0]?.bins[0], 1);
  assert.equal(histogram.value.series[2]?.bins[255], 1);
  const metric = errorMetricOf(hdr, { ...hdr, rgba: new Float32Array([1, 8, 8, 1]) }, 'rgba');
  assert.equal(metric.kind === 'available' && metric.value.rmse, Math.sqrt(25 / 3));
  assert.equal(histogramOf({ ...hdr, rgba: new Float32Array([NaN, 4, 8, 1]) }, 'rgba').kind, 'unavailable');
});

test('the mip cache bounds both Source and Result memory and excludes an oversized rendering', () => {
  const base = { ...pixels, rgba: new Uint8Array(PREVIEW_CACHE_BYTES / 2) };
  const first = { source: base, result: base };
  const second = { source: base, result: { ...base, level: 1 } };
  const cached = cachePreview(cachePreview([], first), second);
  assert.deepEqual(cached, [second]);
  assert.deepEqual(cachePreview(cached, { source: base, result: { ...base, rgba: new Uint8Array(PREVIEW_CACHE_BYTES + 1) } }), []);
});
