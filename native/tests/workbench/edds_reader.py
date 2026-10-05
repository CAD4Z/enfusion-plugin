"""An independent reader of ENF1 EDDS files: the DDS header, the mip table and the LZ4 frames.

It uses only Python's standard library and shares no code with the converter or the capture
oracles, so what it reads can stand as evidence against both.
"""

import dataclasses
import pathlib
import struct


class Lz4Frame:
    """One ENF1 LZ4 payload: the decoded size, then blocks that each decode to 64 KiB but the last.

    A block's matches may reach back into the blocks before it, so they all decode into one buffer.
    """

    BLOCK_OUTPUT = 65536
    LAST_BLOCK = 0x80000000

    def __init__(self, frame: bytes):
        self.frame = frame

    def decode(self) -> bytes:
        """The decoded bytes; any departure from the frame layout fails an assertion."""
        expected = struct.unpack_from('<I', self.frame, 0)[0]
        output = bytearray()
        cursor = 4
        last = False

        while cursor < len(self.frame):
            assert not last

            # Each block carries its stored size and, in the top bit, whether it is the last one.
            descriptor = struct.unpack_from('<I', self.frame, cursor)[0]
            last = bool(descriptor & self.LAST_BLOCK)
            size = descriptor & 0x7FFFFFFF
            block = self.frame[cursor + 4 : cursor + 4 + size]
            assert len(block) == size and size > 0
            cursor += 4 + size

            start = len(output)
            self.decode_block(block, output, expected)
            assert len(output) - start == (expected - start if last else self.BLOCK_OUTPUT)

        assert cursor == len(self.frame) and last and len(output) == expected
        return bytes(output)

    @classmethod
    def decode_block(cls, block: bytes, output: bytearray, limit: int) -> None:
        """Appends one LZ4 block's sequences to `output`, which never grows past `limit`."""
        at = 0
        while at < len(block):
            token = block[at]
            at += 1

            # Literals first: their count is the token's high nibble, extended by 255-runs.
            literal, at = cls.length(block, at, token >> 4)
            assert at + literal <= len(block) and len(output) + literal <= limit
            output.extend(block[at : at + literal])
            at += literal

            # The last sequence of a block has no match.
            if at == len(block):
                break

            distance = struct.unpack_from('<H', block, at)[0]
            at += 2
            count, at = cls.length(block, at, token & 15)
            count += 4
            assert 0 < distance <= len(output) and len(output) + count <= limit

            # Byte by byte, since a match may overlap the bytes it is producing.
            for _ in range(count):
                output.append(output[-distance])

    @staticmethod
    def length(block: bytes, at: int, initial: int) -> tuple[int, int]:
        """A length that starts as `initial` and, at 15, adds bytes until one is not 255."""
        total = initial
        if initial == 15:
            while True:
                extra = block[at]
                at += 1
                total += extra
                if extra != 255:
                    break

        return total, at


@dataclasses.dataclass
class EddsLevel:
    """One mip level as stored: its size, container tag and decoded bytes."""

    width: int
    height: int
    container: str
    pixels: bytes


class EddsFile:
    """A DDS header with ENF1 in its reserved words, a mip table and the stored levels."""

    HEADER_SIZE = 128

    def __init__(self, data: bytes):
        self.data = data
        assert data[:4] == b'DDS ' and self.word(4) == 124
        assert data[36:40] == b'ENF1'

    @classmethod
    def read(cls, path: pathlib.Path) -> 'EddsFile':
        """The file at `path`."""
        return cls(pathlib.Path(path).read_bytes())

    def word(self, offset: int) -> int:
        """The little-endian 32-bit word at `offset`."""
        return struct.unpack_from('<I', self.data, offset)[0]

    @property
    def width(self) -> int:
        return self.word(16)

    @property
    def height(self) -> int:
        return self.word(12)

    @property
    def mip_count(self) -> int:
        return self.word(28)

    def levels(self) -> list[EddsLevel]:
        """Every level, largest first, each decoded from its container.

        The table after the header lists the levels smallest first, a tag and a stored size each,
        and the payloads follow it in the same order up to the end of the file.
        """
        count = self.mip_count
        cursor = self.HEADER_SIZE + count * 8
        stored = []
        for entry in range(count):
            tag = self.data[self.HEADER_SIZE + entry * 8 : self.HEADER_SIZE + entry * 8 + 4]
            size = self.word(self.HEADER_SIZE + entry * 8 + 4)
            assert 0 < size <= len(self.data) - cursor
            assert tag in (b'COPY', b'LZ4 ')
            payload = self.data[cursor : cursor + size]
            cursor += size
            stored.append((tag.decode().strip(), payload if tag == b'COPY' else Lz4Frame(payload).decode()))

        assert cursor == len(self.data)

        levels = []
        for level, (container, pixels) in enumerate(reversed(stored)):
            levels.append(EddsLevel(max(self.width >> level, 1), max(self.height >> level, 1), container, pixels))

        return levels
