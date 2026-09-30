"""Version-pinned swizzle oracle; invoke with the same EXE and output arguments as capture.py."""
from capture import Oracle, pixels_of, fingerprint
import json
import pathlib
import struct
import sys

MAPPINGS = [
    (1, 'TerrainLayerTexture', 'terrain-layer-texture'),
    (2, 'TerrainSuperTexture', 'terrain-super-texture'),
    (7, 'TerrainNormalSpecular_SYxX', 'terrain-normal-specular-syxx'),
    (3, 'AlphaToRGB', 'alpha-to-rgb'),
    (4, 'SMDIToGS', 'smdi-to-gs'),
    (5, 'NormalMap_NOHQ', 'normal-map-nohq'),
    (6, 'NormalMapGA', 'normal-map-ga'),
    (8, 'NormalSpecularMapXYZS', 'normal-specular-map-xyzs'),
    (9, 'AmbientSpecularMapGA', 'ambient-specular-map-ga'),
]


class SwizzleOracle(Oracle):
    def __init__(self):
        super().__init__()
        # Capture the exact BC encoder input and requested DXGI format. No swizzle/mip arithmetic
        # is replaced. The lossy encoder is tested independently; these bytes are not BC output.
        self.hook(0x140f4cdb0, self.capture_encoder)
        self.hook(0x140f497f0, self.capture_channels)
        self.encoder_format = None

    def capture_encoder(self):
        source, output, params = [self.arg(i) for i in range(3)]
        self.encoder_format = struct.unpack('<I', self.uc.mem_read(params, 4))[0]
        return self.capture_pixels(source, output)

    def capture_channels(self):
        self.encoder_format = self.arg(2)
        return self.capture_pixels(self.arg(0), self.arg(1))

    def capture_pixels(self, source, output):
        img = self.images[source]
        self.image(output, img['width'], img['height'], img['format'], count=len(img['levels']))
        for src, dst in zip(img['levels'], self.images[output]['levels']):
            self.uc.mem_write(dst[2], bytes(self.uc.mem_read(src[2], src[3])))
        return 0


if __name__ == '__main__':
    rows = []
    for enum, name, wire in MAPPINGS:
        variants = [
            dict(width=4, height=4),
            dict(width=7, height=5),
            dict(width=1, height=7),
            dict(width=7, height=1),
            dict(width=1, height=1),
            dict(width=8, height=8, alpha=False),
            dict(width=8, height=8, opaque=True),
            dict(width=8, height=8, generate=False),
            dict(width=8, height=8, normalize=True),
            dict(width=8, height=8, mip_function=1),
            dict(width=7, height=5, mip_filter=2, tiled=False),
            dict(width=8, height=8, mip_filter=2, tiled=True),
            dict(width=8, height=8, conversion=1),
            dict(width=8, height=8, conversion=1, alpha=False),
            dict(width=8, height=8, conversion=1, opaque=True),
            *[dict(width=8, height=8, conversion=c) for c in [2,3,4,6,8]],
            *[dict(width=16, height=16, conversion=c, remove=1) for c in [2,3,4,6,8]],
            dict(width=32, height=16, remove=1),
            dict(width=32, height=32, remove=2),
            dict(width=32, height=32, remove=1, normalize=True),
            dict(width=32, height=32, remove=1, mip_filter=2, tiled=False),
        ]
        for variant in variants:
            args = dict(width=8, height=8, alpha=True, opaque=False, generate=True,
                        normalize=False, mip_function=0, mip_filter=0, tiled=True, remove=0, conversion=0)
            args.update(variant)
            width, height = args['width'], args['height']
            pixels = pixels_of(width, height, args['alpha'] and not args['opaque'])
            if args['conversion'] != 0:
                # A smooth asymmetric colour/alpha ramp gives a meaningful tight BC error bound.
                pixels = bytes(v for y in range(height) for x in range(width) for v in
                    [20 + x * 4 + y * 2, 40 + x * 2 + y * 3,
                        200 - x * 5 - y * 2, 20 + x * 2 + y * 3 if args['alpha'] and not args['opaque'] else 255])
            call_args = {k:v for k,v in args.items() if k not in ['alpha', 'opaque']}
            call_args.update(pixels=pixels, fmt=87 if args['alpha'] else 88, swizzling=enum)
            fresh = SwizzleOracle()
            levels = fresh.convert(**call_args)
            reused = SwizzleOracle()
            for repeat in range(2):
                assert reused.convert(**call_args) == levels
                assert reused.output_format == fresh.output_format
                assert reused.encoder_format == fresh.encoder_format
            rows.append(dict(**args, swizzling=name, wire=wire, source_bgra=pixels.hex(),
                             output_format=fresh.output_format, encoder_format=fresh.encoder_format, levels=levels))
            print(name, variant, 'repeatable', flush=True)
    pathlib.Path(sys.argv[2]).write_text(json.dumps(dict(
        workbench_sha256=fingerprint, rows=rows,
        repeatability='Each capture once fresh and twice in a reused emulator; all three outputs identical.',
        compression_boundary='encoder_format is the requested DXGI format; compressed captures contain pre-encoder BGRA, not BC bytes.'
    ), indent=2) + '\n')
