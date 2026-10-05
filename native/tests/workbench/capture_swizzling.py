"""Capture the installed importer's swizzle mappings for owned sources; see README.md.

usage: py capture_swizzling.py WORKBENCH_EXE OUTPUT_JSON
"""

import struct
import sys

from oracle import Capture, Oracle, SourcePattern, WorkbenchExecutable


class SwizzleOracle(Oracle):
    """The importer stopped at its final channel or BC encoder, keeping that boundary's input.

    No swizzle or mip arithmetic is replaced. The lossy encoder is tested independently, so these
    bytes are the encoder's input and requested DXGI format, not BC output.
    """

    ENCODE = 0x140F4CDB0
    PACK_CHANNELS = 0x140F497F0

    def __init__(self, executable: WorkbenchExecutable):
        super().__init__(executable)
        self.hook(self.ENCODE, self.capture_encoder)
        self.hook(self.PACK_CHANNELS, self.capture_channels)
        self.encoder_format = None

    def capture_encoder(self) -> int:
        """The BC encoder: its format is the first word of the parameters in the third argument."""
        source, output, parameters = [self.arg(index) for index in range(3)]
        self.encoder_format = struct.unpack('<I', self.uc.mem_read(parameters, 4))[0]
        return self.capture_pixels(source, output)

    def capture_channels(self) -> int:
        """The channel packer: its format is the third argument."""
        self.encoder_format = self.arg(2)
        return self.capture_pixels(self.arg(0), self.arg(1))

    def capture_pixels(self, source: int, output: int) -> int:
        """Copies the source image's levels into the output image unchanged."""
        image = self.images[source]
        self.image(output, image['width'], image['height'], image['format'], count=len(image['levels']))
        for source_level, output_level in zip(image['levels'], self.images[output]['levels']):
            self.uc.mem_write(output_level[2], bytes(self.uc.mem_read(source_level[2], source_level[3])))

        return 0


class SwizzleCapture(Capture):
    """Every mapping over the same variants, each once fresh and twice in a reused emulator."""

    # (Workbench enum, Workbench name, CLI wire name)
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

    DEFAULTS = dict(
        width=8,
        height=8,
        alpha=True,
        opaque=False,
        generate=True,
        normalize=False,
        mip_function=0,
        mip_filter=0,
        tiled=True,
        remove=0,
        conversion=0,
    )

    VARIANTS = [
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
        *[dict(width=8, height=8, conversion=conversion) for conversion in [2, 3, 4, 6, 8]],
        *[dict(width=16, height=16, conversion=conversion, remove=1) for conversion in [2, 3, 4, 6, 8]],
        dict(width=32, height=16, remove=1),
        dict(width=32, height=32, remove=2),
        dict(width=32, height=32, remove=1, normalize=True),
        dict(width=32, height=32, remove=1, mip_filter=2, tiled=False),
    ]

    def capture(self) -> dict:
        return dict(
            workbench_sha256=self.executable.fingerprint,
            rows=self.rows(),
            repeatability='Each capture once fresh and twice in a reused emulator; all three outputs identical.',
            compression_boundary='encoder_format is the requested DXGI format; compressed captures contain pre-encoder BGRA, not BC bytes.',
        )

    @staticmethod
    def source(width: int, height: int, alpha: bool, compressed: bool) -> bytes:
        """The source pixels: the high-frequency pattern, or for a compressed conversion a smooth,
        asymmetric colour and alpha ramp that gives a meaningful, tight BC error bound."""
        if not compressed:
            return SourcePattern.bgra(width, height, alpha)

        return bytes(
            value
            for y in range(height)
            for x in range(width)
            for value in [20 + x * 4 + y * 2, 40 + x * 2 + y * 3, 200 - x * 5 - y * 2, 20 + x * 2 + y * 3 if alpha else 255]
        )

    def rows(self) -> list[dict]:
        """One row per mapping and variant."""
        rows = []
        for enum, name, wire in self.MAPPINGS:
            for variant in self.VARIANTS:
                rows.append(self.row(enum, name, wire, {**self.DEFAULTS, **variant}))
                print(name, variant, 'repeatable', flush=True)

        return rows

    def row(self, enum: int, name: str, wire: str, variant: dict) -> dict:
        """One mapping and variant, after checking that it repeats."""
        pixels = self.source(variant['width'], variant['height'], variant['alpha'] and not variant['opaque'], variant['conversion'] != 0)
        arguments = {key: value for key, value in variant.items() if key not in ['alpha', 'opaque']}
        arguments.update(pixels=pixels, fmt=87 if variant['alpha'] else 88, swizzling=enum)

        fresh = SwizzleOracle(self.executable)
        levels = fresh.convert(**arguments)
        reused = SwizzleOracle(self.executable)
        for _ in range(2):
            assert reused.convert(**arguments) == levels
            assert reused.output_format == fresh.output_format
            assert reused.encoder_format == fresh.encoder_format

        return dict(
            **variant,
            swizzling=name,
            wire=wire,
            source_bgra=pixels.hex(),
            output_format=fresh.output_format,
            encoder_format=fresh.encoder_format,
            levels=levels,
        )


if __name__ == '__main__':
    sys.exit(SwizzleCapture.main(sys.argv[1:], __doc__))
