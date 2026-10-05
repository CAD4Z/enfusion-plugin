"""Capture the installed HDR decoder, float importer, cubemap projection and container writer.

The final BC encoder boundary is recorded; these are not Workbench BC6H blocks. See
hdr-evidence.md.

usage: py capture_hdr.py WORKBENCH_EXE OUTPUT_JSON
"""

import math
import re
import struct
import sys

from unicorn.x86_const import UC_X86_REG_RAX

from oracle import Capture, Oracle, WorkbenchExecutable


class HdrOracle(Oracle):
    """Float sources, cube faces, the Radiance decoder and the float container writer."""

    # The BC encoder, intercepted at its input; the Radiance decoder; the float container writer.
    ENCODE = 0x140F4CDB0
    DECODE_RADIANCE = 0x140D88630
    WRITE_CONTAINER = 0x140429A30

    # Formats of the image object: RGBA32F, and BC6H in 4x4 blocks of 16 bytes.
    RGBA32F = 2
    BC6H = 95

    def __init__(self, executable: WorkbenchExecutable):
        super().__init__(executable)
        self.hook(self.ENCODE, self.capture_encoder)
        self.hook(0x14042FE90, lambda: self.virtual(0x50))  # lock a face's level
        self.hook(0x14042FF00, lambda: 1)
        self.hook(0x14180CBC0, lambda: 0)  # stack probing only
        self.encoder = None

    def import_call(self, name: str) -> int:
        """The heap, string and float imports the HDR paths add to the base set."""
        if name == 'GetProcessHeap':
            return 1
        if name == 'HeapAlloc':
            return self.alloc(self.arg(2))
        if name == 'HeapFree':
            return 1
        if name == 'HeapSize':
            return self.allocations[self.arg(2)]

        if name == 'ldexp':
            exponent = self.arg(1) & 0xFFFFFFFF
            exponent = exponent if exponent < 0x80000000 else exponent - 0x100000000
            return self.return_double(math.ldexp(self.xmm_doubles()[0], exponent))

        if name in ['strcmp', 'strncmp']:
            limit = self.arg(2) if name == 'strncmp' else 4096
            a, b = self.c_string(self.arg(0), limit), self.c_string(self.arg(1), limit)
            return (a > b) - (a < b)

        if name == 'strtol':
            text = bytes(self.uc.mem_read(self.arg(0), 200)).split(b'\0')[0]
            match = re.match(rb'\s*[+-]?\d+', text)
            if self.arg(1):
                self.write64(self.arg(1), self.arg(0) + (len(match[0]) if match else 0))
            return int(match[0]) if match else 0

        if name == '_hypotf':
            return self.return_float(math.hypot(*self.xmm_floats()))

        return super().import_call(name)

    def c_string(self, address: int, limit: int) -> bytes:
        """The NUL-terminated bytes at `address`, at most `limit` of them."""
        text = bytearray()
        for offset in range(limit):
            value = bytes(self.uc.mem_read(address + offset, 1))[0]
            if not value:
                break
            text.append(value)

        return bytes(text)

    def decode_hdr(self, data: bytes) -> tuple | None:
        """The installed Radiance decoder on `data`: (width, height, channels, float samples), or
        None when it refuses the file."""
        buffer = self.alloc(len(data))
        self.uc.mem_write(buffer, data)

        # A read context over the whole buffer, and room for the three dimensions it reports.
        context = self.alloc(0xE0)
        for offset, value in [(0xB8, buffer), (0xC0, buffer + len(data)), (0xC8, buffer), (0xD0, buffer + len(data))]:
            self.write64(context + offset, value)
        dimensions = self.alloc(32)

        self.call(
            self.DECODE_RADIANCE,
            [context, dimensions, dimensions + 4, dimensions + 8],
            count=1000000,
            stack={0x28: 3, 0x30: dimensions + 16},
        )
        samples = self.uc.reg_read(UC_X86_REG_RAX)
        if not samples:
            return None

        width, height, channels = struct.unpack('<3I', self.uc.mem_read(dimensions, 12))
        count = width * height * channels
        return width, height, channels, list(struct.unpack('<' + 'f' * count, self.uc.mem_read(samples, count * 4)))

    def convert(self, *arguments, **options) -> list[dict]:
        """As the base, remembering which image the container writer should serialize."""
        levels = super().convert(*arguments, **options)
        if self.encoder is None:
            # Conversion=None never reaches the encoder: the importer's own output is serialized.
            self.output_address = self.converted_address
        return levels

    def write_container(self) -> bytes:
        """The installed float container writer on `output_address`, into a memory stream."""
        stream = self.alloc(64)
        vtable = self.alloc(64)
        self.write64(stream, vtable)
        self.write64(vtable + 8, self.stub(lambda: 1))

        parts = []

        def write():
            count = self.arg(2)
            parts.append(bytes(self.uc.mem_read(self.arg(1), count)))
            return count

        self.write64(vtable + 0x28, self.stub(write))

        source = self.output_address
        parameters = self.alloc(32)
        self.uc.mem_write(parameters, struct.pack('<4I', 0, 0, 0, self.images[source]['count']))
        self.call(self.WRITE_CONTAINER, [stream, source, parameters], count=10000000)
        return b''.join(parts)

    def image(self, address=None, width=0, height=0, fmt=87, pixels=None, count=1, faces=1) -> int:
        """As the base, with `faces` chains of levels and the image header the HDR paths read."""
        address = address or self.alloc(0x80)
        self.write64(address, self.vtable)
        self.uc.mem_write(address + 8, struct.pack('<IHHHBB', fmt, width, height, 1, count, faces))

        levels = []
        for _ in range(faces):
            level_width, level_height = width, height
            for level in range(count):
                size = self.level_size(fmt, level_width, level_height)
                buffer = self.alloc(size)
                if pixels is not None and level == 0:
                    self.uc.mem_write(buffer, pixels)
                levels.append((level_width, level_height, buffer, size))
                level_width, level_height = max(level_width // 2, 1), max(level_height // 2, 1)

        self.images[address] = dict(width=width, height=height, format=fmt, levels=levels, count=count, faces=faces)
        return address

    @classmethod
    def level_size(cls, fmt: int, width: int, height: int) -> int:
        """Bytes of one level: BC6H blocks, RGBA32F texels, or BGRA8 texels."""
        if fmt == cls.BC6H:
            return ((width + 3) // 4) * ((height + 3) // 4) * 16
        return width * height * (16 if fmt == cls.RGBA32F else 4)

    @classmethod
    def row_pitch(cls, fmt: int, width: int) -> int:
        """Bytes of one row: of BC6H blocks, RGBA32F texels, or BGRA8 texels."""
        if fmt == cls.BC6H:
            return ((width + 3) // 4) * 16
        return width * (16 if fmt == cls.RGBA32F else 4)

    def virtual(self, slot: int) -> int:
        """The image methods that know about faces; the rest as the base."""
        address = self.arg(0)
        image = self.images[address]

        if slot == 0x80:
            return 1
        if slot == 0x88:
            return image['faces']
        if slot == 0x90:
            return image['count']
        if slot == 0xA0:
            return int(image['faces'] == 6)

        if slot == 0x40:
            # (width, height, array size, mips, format, flags, ...): flag 0x1000 makes a six-face
            # cube whatever array size it is created with; the importer's None cube passes size 1.
            faces = 6 if self.arg(6) & 0x1000 else self.arg(3) & 0xFFFFFFFF
            self.image(address, self.arg(1), self.arg(2), self.arg(5) & 0xFFFFFFFF, count=self.arg(4) & 0xFFFFFFFF, faces=faces)
            return 1

        if slot == 0x50:
            face, level = self.arg(1), self.arg(2)
            level_width, _, pixels, _ = image['levels'][face * image['count'] + level]
            self.uc.mem_write(self.arg(3), struct.pack('<I4xQ', self.row_pitch(image['format'], level_width), pixels))
            return 1

        return super().virtual(slot)

    def capture_encoder(self) -> int:
        """Records the encoder's format and quality, and passes its float input on unchanged."""
        source, output, parameters = [self.arg(index) for index in range(3)]
        self.encoder = dict(
            format=struct.unpack('<I', self.uc.mem_read(parameters, 4))[0],
            quality=struct.unpack('<f', self.uc.mem_read(parameters + 24, 4))[0],
        )

        image = self.images[source]
        self.image(output, image['width'], image['height'], image['format'], count=image['count'], faces=image['faces'])
        for source_level, output_level in zip(image['levels'], self.images[output]['levels']):
            self.uc.mem_write(output_level[2], bytes(self.uc.mem_read(source_level[2], source_level[3])))

        self.output_address = output
        return 0


class EncoderOptions(HdrOracle):
    """Runs the installed BC6H encoder itself, unintercepted, until it starts its worker threads.

    The encoder hands its codec named options as strings before any block is encoded; those strings
    are recorded. Its thread pool cannot run in a single-threaded emulator, so the run stops there.
    """

    class Started(Exception):  # noqa: N818 - a signal that ends the run, not an error
        """The encoder started its first worker thread."""

    FORMAT = re.compile(r'(%[-+ #0]*\d*(?:\.\d+)?[dusf])')

    def __init__(self, executable: WorkbenchExecutable):
        self.strings = []
        super().__init__(executable)

    def hook(self, address: int, answer) -> None:
        """Every hook but the encoder's own."""
        if address != self.ENCODE:
            super().hook(address, answer)

    def import_call(self, name: str) -> int:
        """The formatting, floating-point control and threading imports the encoder makes."""
        if name == '__stdio_common_vsprintf':
            text = self.vsprintf(bytes(self.uc.mem_read(self.arg(3), 200)).split(b'\0')[0].decode(), self.arg(5))
            self.strings.append(text)
            data = text.encode() + b'\0'
            if self.arg(1) and self.arg(2) >= len(data):
                self.uc.mem_write(self.arg(1), data)
            return len(text)

        if name == '_controlfp_s':
            if self.arg(0):
                self.uc.mem_write(self.arg(0), struct.pack('<I', 0x9001F))
            return 0

        if name in ['_Cnd_init', '_Mtx_init']:
            self.write64(self.arg(0), self.alloc(64))
            return 0

        if name in ['_Mtx_lock', '_Mtx_unlock']:
            return 0

        if name == '_Thrd_start':
            raise EncoderOptions.Started()

        return super().import_call(name)

    def vsprintf(self, fmt: str, arguments: int) -> str:
        """printf of the %d, %u, %s and %f conversions the encoder uses, over a va_list."""
        text, index = '', 0
        for piece in self.FORMAT.split(fmt):
            if not piece.startswith('%'):
                text += piece
                continue

            raw = self.read64(arguments + index * 8)
            index += 1
            if piece[-1] == 'f':
                text += piece % struct.unpack('<d', struct.pack('<Q', raw))[0]
            elif piece[-1] == 's':
                text += bytes(self.uc.mem_read(raw, 200)).split(b'\0')[0].decode()
            else:
                value = raw & 0xFFFFFFFF
                text += piece % (value - (1 << 32) if piece[-1] == 'd' and value >= 0x80000000 else value)

        return text


class HdrCapture(Capture):
    """The decoder, importer, writer and encoder evidence for HDR sources."""

    # Workbench's Conversion enum: None, and HDRCompression.
    NONE = 0
    HDR_COMPRESSION = 7

    EVIDENCE = (
        'Installed Radiance decoder and importer execute unchanged. HDRCompression encoder format and quality are intercepted; '
        "None output is the importer's own. Float containers are serialized by the installed writer. Each importer case once "
        'fresh and twice reused. encoder_options are the strings the unintercepted encoder passes to its codec before its '
        'worker threads start.'
    )

    def capture(self) -> dict:
        decoder = self.decoder_rows()
        return dict(
            workbench_sha256=self.executable.fingerprint,
            decoder=decoder,
            rows=self.importer_rows(decoder[0]['rgb']),
            containers=self.bc6_containers(),
            dimensions=self.dimensions(),
            panoramas=self.panoramas(),
            above_half=self.above_half(),
            encoder_options=self.encoder_options(),
            evidence=self.EVIDENCE,
        )

    def oracle(self) -> HdrOracle:
        """A fresh emulator."""
        return HdrOracle(self.executable)

    @staticmethod
    def float_pixels(rgb: list[float]) -> bytes:
        """RGB samples as opaque RGBA32F."""
        return b''.join(struct.pack('<4f', *rgb[at : at + 3], 1) for at in range(0, len(rgb), 3))

    @staticmethod
    def levels_of(levels: list[dict]) -> list[dict]:
        """Importer levels as the file records them: the BGRA key holds RGBA32F samples here."""
        return [dict(width=level['width'], height=level['height'], rgba=level['bgra']) for level in levels]

    def rgbe_source(self, width: int, height: int, exponent=lambda x, y: 132) -> tuple[bytes, bytes]:
        """An owned flat Radiance file, and the RGBA32F samples the installed decoder reads."""
        pixels = bytes(
            value
            for y in range(height)
            for x in range(width)
            for value in [
                128 | (x * 37 + y * 11) % 128,
                128 | (y * 29 + x * 7) % 128,
                128 | (x * 5 + y * 3) % 128,
                exponent(x, y),
            ]
        )
        source = f'#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y {height} +X {width}\n'.encode() + pixels
        return source, self.float_pixels(self.oracle().decode_hdr(source)[3])

    def accepted(self, width: int, height: int, pixels: bytes, **profile) -> list[dict] | None:
        """The importer's levels for this float source and profile, or None when it refuses them."""
        try:
            return self.oracle().convert(width, height, pixels, fmt=HdrOracle.RGBA32F, **profile)
        except RuntimeError as error:
            if 'importer refused fixture' not in str(error):
                raise
            return None

    def decoder_rows(self) -> list[dict]:
        """One 16x8 image, flat and as RLE scanlines, under both signatures, decoded twice."""
        width, height = 16, 8
        pixels = bytes(value for y in range(height) for x in range(width) for value in [64 + x * 4, 32 + y * 8, 16 + x + y, 132])

        rows = []
        for rle in [False, True]:
            payload = pixels
            if rle:
                # A new-style scanline: its marker, then each channel as one literal run.
                payload = b''.join(
                    bytes([2, 2, 0, width])
                    + b''.join(bytes([width]) + pixels[(y * width) * 4 + channel : (y * width + width) * 4 : 4] for channel in range(4))
                    for y in range(height)
                )

            for signature in ['RADIANCE', 'RGBE']:
                source = f'#?{signature}\nFORMAT=32-bit_rle_rgbe\n\n-Y {height} +X {width}\n'.encode() + payload
                decoded = self.oracle().decode_hdr(source)
                assert decoded == self.oracle().decode_hdr(source)
                rows.append(dict(source=source.hex(), width=width, height=height, rgb=decoded[3]))

        return rows

    def importer_cases(self) -> list[tuple]:
        """(conversion, cube, generate, quality, filter, tiled) for every importer row.

        HDRCompression at three qualities with the first two filter and tiling pairs; then the other
        two pairs, and Conversion=None, whose RGBA32F output the importer itself produces.
        """
        cases = [
            (self.HDR_COMPRESSION, cube, generate, quality, mip_filter, tiled)
            for cube in [False, True]
            for generate in [False, True]
            for quality in [0, 0.403, 1]
            for mip_filter, tiled in [(0, True), (2, False)]
        ]
        cases += [
            (self.HDR_COMPRESSION, cube, True, 1, mip_filter, tiled)
            for cube in [False, True]
            for mip_filter, tiled in [(0, False), (2, True)]
        ]
        cases += [
            (self.NONE, cube, generate, 1, mip_filter, tiled)
            for cube in [False, True]
            for generate in [False, True]
            for mip_filter, tiled in ([(0, True), (2, False), (0, False), (2, True)] if generate else [(0, True)])
        ]
        return cases

    def importer_rows(self, rgb: list[float]) -> list[dict]:
        """Each case's encoder input or importer output, and the installed writer's container."""
        width, height = 16, 8
        pixels = self.float_pixels(rgb)

        rows = []
        for conversion, cube, generate, quality, mip_filter, tiled in self.importer_cases():
            profile = dict(
                fmt=HdrOracle.RGBA32F,
                conversion=conversion,
                cubemap=cube,
                generate=generate,
                quality=quality,
                mip_filter=mip_filter,
                tiled=tiled,
            )
            oracle = self.oracle()
            levels = oracle.convert(width, height, pixels, **profile)
            reused = self.oracle()
            for _ in range(2):
                assert reused.convert(width, height, pixels, **profile) == levels

            container = oracle.write_container()
            rows.append(
                dict(
                    width=width,
                    height=height,
                    conversion=conversion,
                    cubemap=cube,
                    generate=generate,
                    quality=quality,
                    filter=mip_filter,
                    tiled=tiled,
                    encoder=oracle.encoder,
                    output_format=oracle.output_format,
                    source_rgba=pixels.hex(),
                    levels=self.levels_of(levels),
                    float_container=container.hex(),
                )
            )
            print(conversion, cube, generate, quality, mip_filter, tiled, flush=True)

        return rows

    def bc6_containers(self) -> list[dict]:
        """The installed writer's BC6H container for a 4x4, three-level image and cube."""
        containers = []
        for faces in [1, 6]:
            oracle = self.oracle()
            oracle.output_address = oracle.image(width=4, height=4, fmt=HdrOracle.BC6H, count=3, faces=faces)
            containers.append(dict(faces=faces, bc6_container=oracle.write_container().hex()))

        return containers

    def dimensions(self) -> list[dict]:
        """Which source sizes the importer accepts.

        HDRCompression needs power-of-two sides of at least four; None takes any size, so its
        levels are recorded too.
        """
        rows = []
        for conversion in [self.HDR_COMPRESSION, self.NONE]:
            for width, height in [(4, 4), (8, 4), (4, 8), (16, 8), (12, 8), (6, 6), (3, 5), (2, 2), (1, 1)]:
                source, pixels = self.rgbe_source(width, height)
                levels = self.accepted(width, height, pixels, conversion=conversion, generate=True)
                row = dict(conversion=conversion, width=width, height=height, source=source.hex(), accepted=levels is not None)
                if levels is not None and conversion == self.NONE:
                    row.update(levels=self.levels_of(levels))
                rows.append(row)
                print('dimensions', conversion, width, height, row['accepted'], flush=True)

        return rows

    def panoramas(self) -> list[dict]:
        """Which panorama sizes the importer projects onto a cube."""
        rows = []
        for width, height in [(16, 8), (32, 16), (24, 12), (8, 4), (32, 8), (16, 16)]:
            source, pixels = self.rgbe_source(width, height)
            levels = self.accepted(width, height, pixels, conversion=self.HDR_COMPRESSION, cubemap=True, generate=False)
            rows.append(dict(width=width, height=height, source=source.hex(), accepted=levels is not None))
            print('panorama', width, height, levels is not None, flush=True)

        return rows

    def above_half(self) -> dict:
        """Radiance above the half-float range reaches the encoder unchanged; it saturates there.

        The two left columns have exponent 146: 2^17 and more.
        """
        source, pixels = self.rgbe_source(16, 8, lambda x, y: 146 if x < 2 else 132)
        levels = self.oracle().convert(16, 8, pixels, fmt=HdrOracle.RGBA32F, conversion=self.HDR_COMPRESSION, generate=False)
        reached = struct.unpack('<' + 'f' * (16 * 8 * 4), bytes.fromhex(levels[0]['bgra']))

        row = dict(source=source.hex(), source_max=max(struct.unpack('<' + 'f' * (16 * 8 * 4), pixels)), encoder_input_max=max(reached))
        print('above half', row['source_max'], row['encoder_input_max'], flush=True)
        return row

    def encoder_options(self) -> dict:
        """The option strings the installed encoder hands its codec at ConversionQuality 0.403."""
        oracle = EncoderOptions(self.executable)
        try:
            oracle.convert(
                4, 4, self.rgbe_source(4, 4)[1], fmt=HdrOracle.RGBA32F, conversion=self.HDR_COMPRESSION, generate=False, quality=0.403
            )
            raise RuntimeError('the encoder returned without starting its workers')
        except EncoderOptions.Started:
            pass

        options = dict(zip(oracle.strings[::2], oracle.strings[1::2]))
        print('encoder options', options, flush=True)
        return options


if __name__ == '__main__':
    sys.exit(HdrCapture.main(sys.argv[1:], __doc__))
