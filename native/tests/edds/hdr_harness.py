"""What the HDR tests and fixtures share: the executable under test, owned Radiance sources, the
captured goldens, and an independent reader of what the executable writes.

Standard library only. The reader decodes from the formats' specifications, never with the
converter's own code.
"""

import json
import pathlib
import struct
import subprocess


class Converter:
    """The executable under test, run as `enfusion edds COMMAND` with machine-readable output."""

    MACHINE = ('--machine', '--protocol', '1')

    def __init__(self, path: str):
        self.path = path

    def run(self, command: str, *arguments: str) -> subprocess.CompletedProcess:
        """One command; its exit code, stdout and stderr are for the caller to judge."""
        return subprocess.run([self.path, 'edds', command, *self.MACHINE, *arguments], capture_output=True, text=True)

    def convert(self, source: pathlib.Path, output: pathlib.Path, *profile: str) -> subprocess.CompletedProcess:
        """Converts `source` into `output` with the given profile flags."""
        return self.run('convert', '--input', str(source), '--output', str(output), *profile)

    def inspect(self, path: pathlib.Path, *arguments: str) -> subprocess.CompletedProcess:
        """Inspects the EDDS at `path`."""
        return self.run('inspect', '--input', str(path), *arguments)

    def preview(self, path: pathlib.Path, *arguments: str) -> subprocess.CompletedProcess:
        """Previews a level of the EDDS at `path`."""
        return self.run('preview', '--input', str(path), *arguments)


class RadianceFile:
    """Owned Radiance (.hdr) sources."""

    @staticmethod
    def flat(width: int, height: int, rgbe: bytes) -> bytes:
        """A flat, uncompressed Radiance file of `width` x `height` RGBE texels."""
        return f'#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y {height} +X {width}\n'.encode() + rgbe

    @classmethod
    def constant(cls) -> bytes:
        """A 4x4 image of one colour, (8, 4, 2): exactly one BC6H block."""
        return cls.flat(4, 4, bytes([128, 64, 32, 132]) * 16)


class HdrGoldens:
    """The evidence the tests hold the executable to."""

    TESTS = pathlib.Path(__file__).resolve().parents[1]

    @classmethod
    def workbench(cls) -> dict:
        """What the installed Workbench decoder, importer and writer produced (capture_hdr.py)."""
        return json.loads((cls.TESTS / 'workbench' / 'hdr-goldens.json').read_text(encoding='utf-8'))

    @classmethod
    def bc6_blocks(cls) -> list[dict]:
        """BC6H blocks of every mode, each with the RGB samples an independent decoder reads."""
        return json.loads((cls.TESTS / 'edds' / 'bc6-goldens.json').read_text(encoding='utf-8'))['rows']


class Bc6hMode11:
    """Mode 11 of BC6H, decoded from the format's specification alone."""

    # The weights of the 4-bit indices, out of 64, as the format specifies them.
    WEIGHTS = [0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64]

    @staticmethod
    def unquantize(value: int) -> int:
        """A 10-bit endpoint widened to 16 bits."""
        return 0 if value == 0 else 65535 if value == 1023 else ((value << 16) + 32768) >> 10

    @classmethod
    def interpolate(cls, a: int, b: int, weight: int) -> float:
        """The half float between two widened endpoints at `weight` out of 64."""
        bits = (((a * (64 - weight) + b * weight + 32) >> 6) * 31) >> 6
        return struct.unpack('<e', struct.pack('<H', bits))[0]

    @classmethod
    def block(cls, block: bytes) -> list[list[float]]:
        """The 16 RGB samples of one mode-11 block, row by row."""
        bits = int.from_bytes(block, 'little')
        if bits & 31 != 3:
            raise AssertionError(f'expected a mode-11 block, found mode code {bits & 31}')

        # Two RGB endpoints of 10 bits each after the 5-bit mode, then 16 indices from bit 65; the
        # first index has an implicit high bit of zero, so it takes three bits.
        endpoints = [cls.unquantize((bits >> (5 + index * 10)) & 1023) for index in range(6)]
        samples, at = [], 65
        for pixel in range(16):
            width = 3 if pixel == 0 else 4
            weight = cls.WEIGHTS[(bits >> at) & ((1 << width) - 1)]
            at += width
            samples.append([cls.interpolate(endpoints[channel], endpoints[channel + 3], weight) for channel in range(3)])

        return samples

    @classmethod
    def surface(cls, data: bytes, width: int, height: int) -> list[list[float]]:
        """The RGB samples of one surface of 4x4 blocks, row by row, cropped to its size."""
        across = (width + 3) // 4
        rows = [[None] * width for _ in range(height)]
        for block_y in range((height + 3) // 4):
            for block_x in range(across):
                offset = (block_y * across + block_x) * 16
                for index, rgb in enumerate(cls.block(data[offset:][:16])):
                    x, y = block_x * 4 + index % 4, block_y * 4 + index // 4
                    if x < width and y < height:
                        rows[y][x] = rgb

        return [rgb for row in rows for rgb in row]


class CopyEdds:
    """An EDDS whose every level is stored COPY, read from its own header."""

    @staticmethod
    def levels(data: bytes) -> list[tuple[int, int, bytes]]:
        """(width, height, stored bytes) of every level, largest first.

        The mip table follows the DDS header, and the DX10 extension when there is one; it lists
        the levels smallest first, and their payloads follow in the same order.
        """
        header = 148 if data[84:88] == b'DX10' else 128
        height, width = struct.unpack_from('<II', data, 12)
        count = struct.unpack_from('<I', data, 28)[0]

        offset, stored = header + count * 8, {}
        for entry in range(count):
            tag, size = struct.unpack_from('<4sI', data, header + entry * 8)
            assert tag == b'COPY', tag
            stored[count - entry - 1] = data[offset : offset + size]
            offset += size

        assert offset == len(data)
        return [(max(width >> level, 1), max(height >> level, 1), stored[level]) for level in range(count)]

    @staticmethod
    def floats(data: bytes) -> tuple[float, ...]:
        """Little-endian float samples."""
        return struct.unpack('<' + 'f' * (len(data) // 4), data)

    @staticmethod
    def rgb(samples) -> list[float]:
        """RGBA samples without their alpha."""
        return [value for index, value in enumerate(samples) if index % 4 != 3]
