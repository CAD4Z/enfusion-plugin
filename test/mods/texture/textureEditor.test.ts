import assert from 'node:assert/strict';
import { test } from 'node:test';
import type { EddsInspection } from '../../../src/mods/texture/edds';
import {
  RECONVERT_REFUSAL,
  openedTexture,
  updateTexture,
} from '../../../src/mods/texture/textureEditor';

const INSPECTION: EddsInspection = {
  width: 4,
  height: 2,
  pixelFormat: 'BGRA8',
  channels: 'RGBA',
  dds: {
    flags: 0x1_000f,
    pitchOrLinearSize: 16,
    depth: 0,
    pixelFormatFlags: 0x4,
    fourCC: 'ENF1',
    rgbBitCount: 32,
    rMask: 0x00ff_0000,
    gMask: 0x0000_ff00,
    bMask: 0x0000_00ff,
    aMask: 0xff00_0000,
    caps: 0x401008,
    caps2: 0,
    dxgiFormat: 0,
    resourceDimension: 0,
    arraySize: 0,
    miscFlag: 0,
  },
  mips: [
    {
      level: 0,
      width: 4,
      height: 2,
      storedBytes: 32,
      decodedBytes: 32,
      container: 'COPY',
    },
    {
      level: 1,
      width: 2,
      height: 1,
      storedBytes: 8,
      decodedBytes: 8,
      container: 'LZ4',
    },
  ],
  pixels: { kind: 'supported' },
};

test('standalone mip navigation reuses previously decoded pixels', () => {
  const inspected = updateTexture(openedTexture().state, { kind: 'inspected', inspection: INSPECTION });
  const first = updateTexture(inspected.state, { kind: 'previewed', request: 1,
    preview: { level: 0, width: 4, height: 2, rgba: new Uint8Array(32) } });
  const selected = updateTexture(first.state, { kind: 'select-mip', mip: 1 });
  const decoded = updateTexture(selected.state, { kind: 'previewed', request: 2,
    preview: { level: 1, width: 2, height: 1, rgba: new Uint8Array(8) } });
  const back = updateTexture(decoded.state, { kind: 'select-mip', mip: 0 });
  assert.deepEqual(back.effects, []);
  assert.equal(back.state.kind === 'inspect-only' && back.state.preview.kind, 'ready');
});

test('a directly opened EDDS becomes an inspect-only editor and decodes its largest mip', () => {
  const opened = openedTexture();
  assert.deepEqual(opened.effects, [{ kind: 'inspect' }]);
  assert.equal(opened.state.kind, 'loading');

  const inspected = updateTexture(opened.state, { kind: 'inspected', inspection: INSPECTION });

  assert.equal(inspected.state.kind, 'inspect-only');
  if (inspected.state.kind !== 'inspect-only') {
    return;
  }

  assert.equal(inspected.state.readOnly, true);
  assert.equal(inspected.state.reconvert.reason, RECONVERT_REFUSAL);
  assert.equal(inspected.state.selectedMip, 0);
  assert.deepEqual(inspected.effects, [{ kind: 'preview', mip: 0, request: 1 }]);
});

test('an unsupported pixel format keeps every inspected fact without inventing pixels', () => {
  const loading = openedTexture().state;
  const inspection: EddsInspection = {
    ...INSPECTION,
    pixelFormat: 'DXGI_98',
    channels: 'UNKNOWN',
    pixels: { kind: 'unsupported', reason: 'BC7 preview is not supported yet.' },
  };

  const inspected = updateTexture(loading, { kind: 'inspected', inspection });

  assert.deepEqual(inspected.effects, []);
  assert.equal(inspected.state.kind, 'inspect-only');
  if (inspected.state.kind !== 'inspect-only') {
    return;
  }
  assert.strictEqual(inspected.state.inspection, inspection);
  assert.deepEqual(inspected.state.preview, {
    kind: 'unsupported-format',
    reason: 'BC7 preview is not supported yet.',
  });
});

test('channels reuse decoded pixels while a different mip asks only for that mip preview', () => {
  const loading = openedTexture().state;
  const inspected = updateTexture(loading, { kind: 'inspected', inspection: INSPECTION });
  const previewed = updateTexture(inspected.state, {
    kind: 'previewed',
    request: 1,
    preview: {
      level: 0,
      width: 4,
      height: 2,
      rgba: new Uint8Array(32),
    },
  });

  const channel = updateTexture(previewed.state, { kind: 'select-channel', channel: 'alpha' });
  assert.deepEqual(channel.effects, []);
  assert.equal(channel.state.kind === 'inspect-only' && channel.state.channel, 'alpha');

  const mip = updateTexture(channel.state, { kind: 'select-mip', mip: 1 });
  assert.deepEqual(mip.effects, [{ kind: 'preview', mip: 1, request: 2 }]);
  assert.equal(mip.state.kind === 'inspect-only' && mip.state.selectedMip, 1);

  const sameMip = updateTexture(mip.state, { kind: 'select-mip', mip: 1 });
  assert.deepEqual(sameMip.effects, []);
});

test('a late preview response cannot replace the pixels of the newly selected mip', () => {
  const loading = openedTexture().state;
  const inspected = updateTexture(loading, { kind: 'inspected', inspection: INSPECTION });
  const selected = updateTexture(inspected.state, { kind: 'select-mip', mip: 1 });

  const stale = updateTexture(selected.state, {
    kind: 'previewed',
    request: 1,
    preview: { level: 0, width: 4, height: 2, rgba: new Uint8Array(32) },
  });

  assert.strictEqual(stale.state, selected.state);
  assert.deepEqual(stale.effects, []);
});

test('direct preview exposes reconvert only after a validated metadata relationship', () => {
  const loading = openedTexture().state;
  const available = updateTexture(loading, {
    kind: 'reconversion-available',
    source: 'C:\\mod\\Mod\\icon.png',
  });

  assert.deepEqual(available.state.reconvert, {
    kind: 'available',
    source: 'C:\\mod\\Mod\\icon.png',
  });
});
