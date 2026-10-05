import type { TextureRendering } from '../../src/mods/texture/textureAuthoring';

/** Distinct pixels make selecting the wrong cube face observable. */
export function cubeRendering(): TextureRendering {
  const faces = Array.from({ length: 6 }, (_, face) => Uint8Array.of((face + 1) * 10, 0, 0, 255));
  const rgba = Uint8Array.of(10, 0, 0, 255);
  return {
    source: { level: 0, width: 1, height: 1, rgba },
    result: { level: 0, width: 1, height: 1, rgba, faces },
    inspection: {
      width: 1, height: 1, pixelFormat: 'BC6H', channels: 'RGB', pixels: { kind: 'supported' },
      mips: [{ level: 0, width: 1, height: 1, container: 'COPY', storedBytes: 96, decodedBytes: 96 }],
      dds: { flags: 1, pitchOrLinearSize: 16, depth: 0, pixelFormatFlags: 4, fourCC: 'DX10',
        rgbBitCount: 0, rMask: 0, gMask: 0, bMask: 0, aMask: 0, caps: 0x1008,
        caps2: 0xfe00, dxgiFormat: 95, resourceDimension: 3, arraySize: 6, miscFlag: 4 },
    },
  };
}
