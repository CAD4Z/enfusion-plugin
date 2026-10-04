"""Capture the installed Workbench's target dispatch and complete container writer.

Invoke with the same EXE/output arguments as capture.py. Only storage/OS services are supplied;
the target dispatch, DDS headers, container selection and compression execute unchanged.
The LZO writer failure is recorded as a failure, never repaired into a purported golden file.
"""
import hashlib
import json
import pathlib
import struct
import sys

from capture import Oracle, base, fingerprint
from unicorn import UC_HOOK_CODE, UcError
from unicorn.x86_const import (
    UC_X86_REG_RAX, UC_X86_REG_RBP, UC_X86_REG_RCX, UC_X86_REG_RDX,
    UC_X86_REG_R8, UC_X86_REG_R9, UC_X86_REG_R13, UC_X86_REG_RIP, UC_X86_REG_RSP,
)


class TargetOracle(Oracle):
    """A memory stream behind the original metadata-to-container writer."""

    def __init__(self):
        super().__init__()
        self.output = bytearray()
        self.lzo = None
        self.stream_vtable = self.alloc(0xb0)
        for slot in range(0, 0xb0, 8):
            self.w64(self.stream_vtable + slot, self.stub(lambda s=slot: self.stream(s)))
        fs_vtable = self.alloc(0x108)
        self.w64(fs_vtable + 0x100, self.stub(lambda: 1))
        self.fs = self.alloc(0x20)
        self.w64(self.fs, fs_vtable)
        self.hook(0x1405217a0, lambda: self.fs)  # filesystem singleton
        self.hook(0x1402c4040, self.stream_ctor)  # memory stream instead of an OS file
        self.hook(0x1402c4120, lambda: 0)  # stream destruction
        self.hook(0x14050e340, lambda: 0)  # allocation release
        # The emulated stack is already mapped. Preserve the size returned by the CRT probe.
        self.hook(0x14180cbc0, lambda: self.uc.reg_read(UC_X86_REG_RAX))
        self.uc.hook_add(UC_HOOK_CODE, self.before_lzo, begin=0x14042a567, end=0x14042a567)
        self.uc.hook_add(UC_HOOK_CODE, self.after_lzo, begin=0x14042a56c, end=0x14042a56c)

    def stream_ctor(self):
        self.w64(self.arg(0), self.stream_vtable)
        return self.arg(0)

    def stream(self, slot):
        if slot == 8:
            return 1  # stream is writable
        if slot == 0x28:
            self.output.extend(self.uc.mem_read(self.arg(1), self.arg(2)))
            return self.arg(2)
        raise RuntimeError(f'Unhandled stream method {slot:#x}')

    def before_lzo(self, uc, address, size, extra):
        frame = uc.reg_read(UC_X86_REG_RBP)
        length = uc.reg_read(UC_X86_REG_R9)
        self.lzo = dict(
            length_overlaps_table_pointer=length + 4 == frame + 8,
            table_pointer_before=self.r64(frame + 8),
        )

    def after_lzo(self, uc, address, size, extra):
        frame = uc.reg_read(UC_X86_REG_RBP)
        count = self.r64(frame + 4)
        if count > 1024 * 1024:
            raise RuntimeError('Unexpected LZO size in the version-pinned writer')
        self.lzo.update(
            stored_bytes=count,
            table_pointer_after=self.r64(frame + 8),
            codec_payload=bytes(uc.mem_read(uc.reg_read(UC_X86_REG_R13), count)).hex(),
        )

    def invalid(self, uc, access, address, size, value, extra):
        if address == 0 and uc.reg_read(UC_X86_REG_RIP) == 0x14042a573 and self.lzo:
            return False
        return super().invalid(uc, access, address, size, value, extra)

    def write(self, levels, target, compression, threshold, alpha=True):
        self.output = bytearray()
        self.lzo = None
        values = {0x2321ae4: target, 0x2321ae8: compression, 0x2321aec: threshold}
        self.meta = self.alloc(0x80)
        entries = self.alloc(len(values) * 16)
        self.w64(self.meta + 0x40, entries)
        self.present = set(range(len(values)))
        for index, (rva, value) in enumerate(values.items()):
            self.uc.mem_write(base + rva, struct.pack('<I', index))
            value_address = self.alloc(8)
            self.w64(value_address, value)
            self.w64(entries + index * 16 + 8, value_address)
        first = levels[0]
        source = self.image(width=first['width'], height=first['height'],
                            count=len(levels), fmt=87 if alpha else 88)
        for expected, (width, height, address, count) in zip(levels, self.images[source]['levels']):
            samples = bytes.fromhex(expected['bgra'])
            assert (width, height, count) == (expected['width'], expected['height'], len(samples))
            self.uc.mem_write(address, samples)
        stack = 0x20ffff08
        self.w64(stack, self.sentinel)
        self.uc.reg_write(UC_X86_REG_RSP, stack)
        for register, value in zip(
            [UC_X86_REG_RCX, UC_X86_REG_RDX, UC_X86_REG_R8, UC_X86_REG_R9],
            [0, source, 0, self.meta],
        ):
            self.uc.reg_write(register, value)
        try:
            self.uc.emu_start(0x140fcf2f0, self.sentinel, count=100000000)
        except UcError:
            if (self.uc.reg_read(UC_X86_REG_RIP) != 0x14042a573 or not self.lzo or
                    not self.lzo['length_overlaps_table_pointer'] or
                    not self.lzo['table_pointer_before'] or self.lzo['table_pointer_after'] != 0):
                raise
            assert not self.output
            return dict(status='writer-fault', fault_rva='0x42a573',
                        length_overlaps_table_pointer=True, table_pointer_zeroed=True,
                        stored_bytes=self.lzo['stored_bytes'], codec_payload=self.lzo['codec_payload'],
                        file_bytes_written=0)
        if self.uc.reg_read(UC_X86_REG_RIP) != self.sentinel:
            raise RuntimeError('Writer instruction limit')
        if not self.uc.reg_read(UC_X86_REG_RAX) & 255:
            raise RuntimeError('Writer refused the controlled image')
        return dict(status='written', edds=self.output.hex())


def levels_of(width, height, count, alpha, patterned):
    """Each mip has its own planted pixels; this capture tests storage, not mip filtering."""
    levels = []
    for level in range(count):
        pixels = bytes(value for y in range(height) for x in range(width) for value in
                       ((x * 67 + y * 13 + level) % 256 if patterned else 3 + level,
                        (y * 79 + x * 19 + level) % 256 if patterned else 5 + level,
                        (x * 37 + y * 11 + level) % 256 if patterned else 7 + level,
                        (x * 47 + y * 59) % 256 if alpha and patterned else 255))
        levels.append(dict(width=width, height=height, bgra=pixels.hex()))
        width, height = max(width // 2, 1), max(height // 2, 1)
    return levels


if __name__ == '__main__':
    rows = []
    sources = []
    files = {}
    variants = [
        (2, 2, 1, True, False, 80),   # below 64 bytes: every target skips compression
        (4, 4, 1, True, False, 80),   # exactly 64 bytes: compression is attempted
        (8, 8, 1, True, False, 80),   # a small LZO writer failure
        (7, 5, 3, True, True, 80),    # NPOT, unique supplied levels, COPY fallback
        (8, 8, 4, False, False, 100), # BGRX, mixed COPY/compressed levels
        (8, 8, 1, True, False, 0),    # threshold cannot prevent the LZO call/fault
        (256, 65, 1, True, False, 80),# a multi-block LZ4 frame
    ]
    for variant in variants:
        width, height, count, alpha, patterned, threshold = variant
        levels = levels_of(width, height, count, alpha, patterned)
        source_index = len(sources)
        sources.append(dict(alpha=alpha, levels=levels))
        for target, name in [(0, 'EnfusionDDS'), (2, 'EnfusionDDS_LZ0'), (3, 'EnfusionDDS_LZ4')]:
            for compression, mode in enumerate(['Copy', 'Fastest', 'Medium', 'Best']):
                args = (levels, target, compression, threshold, alpha)
                try:
                    result = TargetOracle().write(*args)
                except Exception as error:
                    raise RuntimeError(f'Capture failed: {variant}, {name}, {mode}') from error
                reused = TargetOracle()
                for repeat in range(2):
                    assert reused.write(*args) == result
                if result['status'] == 'written':
                    encoded = result.pop('edds')
                    digest = hashlib.sha256(bytes.fromhex(encoded)).hexdigest()
                    files[digest] = encoded
                    result['edds_sha256'] = digest
                rows.append(dict(target=name, target_enum=target, compression=mode,
                                 threshold=threshold, source=source_index, result=result))
    pathlib.Path(sys.argv[2]).write_text(json.dumps(dict(
        workbench='1.29.163709', executable_sha256=fingerprint,
        method='Unchanged metadata target dispatch and container writer in Unicorn; memory stream IO',
        limitation='LZO codec bytes captured before an actual writer fault are NOT complete EDDS files',
        sources=sources, files=files, rows=rows,
    ), indent=2) + '\n', encoding='utf-8')
    print(f'Captured {len(rows)} target/compression cases (fresh plus two reused executions each).')
