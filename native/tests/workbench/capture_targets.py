"""Capture the installed Workbench's target dispatch and complete container writer.

Only storage and OS services are supplied; the target dispatch, DDS headers, container selection
and compression execute unchanged. The LZO writer failure is recorded as a failure, never
repaired into a purported golden file.

usage: py capture_targets.py WORKBENCH_EXE OUTPUT_JSON
"""

import hashlib
import sys

from unicorn import UC_HOOK_CODE, UcError
from unicorn.x86_const import UC_X86_REG_R9, UC_X86_REG_R13, UC_X86_REG_RAX, UC_X86_REG_RBP, UC_X86_REG_RIP

from oracle import Capture, Oracle, WorkbenchExecutable


class TargetOracle(Oracle):
    """A memory stream behind the original metadata-to-container writer."""

    # The metadata target dispatch, and the registration IDs of the properties it reads.
    WRITE = 0x140FCF2F0
    TARGET = 0x2321AE4
    COMPRESSION = 0x2321AE8
    THRESHOLD = 0x2321AEC

    # Around the LZO call: just before it, just after it, and the instruction that then faults.
    BEFORE_LZO = 0x14042A567
    AFTER_LZO = 0x14042A56C
    LZO_FAULT = 0x14042A573

    def __init__(self, executable: WorkbenchExecutable):
        super().__init__(executable)
        self.output = bytearray()
        self.lzo = None

        # A writable stream whose writes land in `output`.
        self.stream_vtable = self.alloc(0xB0)
        for slot in range(0, 0xB0, 8):
            self.write64(self.stream_vtable + slot, self.stub(lambda slot=slot: self.stream(slot)))

        # A file system singleton that hands out such streams.
        file_system_vtable = self.alloc(0x108)
        self.write64(file_system_vtable + 0x100, self.stub(lambda: 1))
        self.file_system = self.alloc(0x20)
        self.write64(self.file_system, file_system_vtable)

        self.hook(0x1405217A0, lambda: self.file_system)  # filesystem singleton
        self.hook(0x1402C4040, self.stream_constructor)  # memory stream instead of an OS file
        self.hook(0x1402C4120, lambda: 0)  # stream destruction
        self.hook(0x14050E340, lambda: 0)  # allocation release

        # The emulated stack is already mapped. Preserve the size returned by the CRT probe.
        self.hook(0x14180CBC0, lambda: self.uc.reg_read(UC_X86_REG_RAX))

        self.uc.hook_add(UC_HOOK_CODE, self.before_lzo, begin=self.BEFORE_LZO, end=self.BEFORE_LZO)
        self.uc.hook_add(UC_HOOK_CODE, self.after_lzo, begin=self.AFTER_LZO, end=self.AFTER_LZO)

    def stream_constructor(self) -> int:
        """Makes the object in the first argument a memory stream."""
        self.write64(self.arg(0), self.stream_vtable)
        return self.arg(0)

    def stream(self, slot: int) -> int:
        """The stream's virtual methods: whether it is writable, and write."""
        if slot == 8:
            return 1

        if slot == 0x28:
            self.output.extend(self.uc.mem_read(self.arg(1), self.arg(2)))
            return self.arg(2)

        raise RuntimeError(f'Unhandled stream method {slot:#x}')

    def before_lzo(self, uc, address, size, extra) -> None:
        """Records whether the codec's length output overlaps the writer's table pointer."""
        frame = uc.reg_read(UC_X86_REG_RBP)
        length = uc.reg_read(UC_X86_REG_R9)
        self.lzo = dict(length_overlaps_table_pointer=length + 4 == frame + 8, table_pointer_before=self.read64(frame + 8))

    def after_lzo(self, uc, address, size, extra) -> None:
        """Records the codec's output and what became of the table pointer."""
        frame = uc.reg_read(UC_X86_REG_RBP)
        count = self.read64(frame + 4)
        if count > 1024 * 1024:
            raise RuntimeError('Unexpected LZO size in the version-pinned writer')

        self.lzo.update(
            stored_bytes=count,
            table_pointer_after=self.read64(frame + 8),
            codec_payload=bytes(uc.mem_read(uc.reg_read(UC_X86_REG_R13), count)).hex(),
        )

    def invalid(self, uc, access, address, size, value, extra) -> bool:
        """The LZO writer's fault through the zeroed table pointer is expected and quiet."""
        if address == 0 and uc.reg_read(UC_X86_REG_RIP) == self.LZO_FAULT and self.lzo:
            return False

        return super().invalid(uc, access, address, size, value, extra)

    def write(self, levels: list[dict], target: int, compression: int, threshold: int, alpha: bool = True) -> dict:
        """Writes the levels as `target`; returns the file, or the LZO fault as captured."""
        self.output = bytearray()
        self.lzo = None
        self.write_profile({self.TARGET: target, self.COMPRESSION: compression, self.THRESHOLD: threshold})

        # The source image holds exactly the supplied levels.
        first = levels[0]
        source = self.image(width=first['width'], height=first['height'], count=len(levels), fmt=87 if alpha else 88)
        for expected, (width, height, address, count) in zip(levels, self.images[source]['levels']):
            samples = bytes.fromhex(expected['bgra'])
            assert (width, height, count) == (expected['width'], expected['height'], len(samples))
            self.uc.mem_write(address, samples)

        try:
            self.call(self.WRITE, [0, source, 0, self.meta], count=100000000)
        except UcError:
            if not self.is_lzo_fault():
                raise

            assert not self.output
            return dict(
                status='writer-fault',
                fault_rva='0x42a573',
                length_overlaps_table_pointer=True,
                table_pointer_zeroed=True,
                stored_bytes=self.lzo['stored_bytes'],
                codec_payload=self.lzo['codec_payload'],
                file_bytes_written=0,
            )

        if self.uc.reg_read(UC_X86_REG_RIP) != self.SENTINEL:
            raise RuntimeError('Writer instruction limit')
        if not self.uc.reg_read(UC_X86_REG_RAX) & 255:
            raise RuntimeError('Writer refused the controlled image')

        return dict(status='written', edds=self.output.hex())

    def is_lzo_fault(self) -> bool:
        """Whether the run stopped where the codec's length overwrote the table pointer."""
        return (
            self.uc.reg_read(UC_X86_REG_RIP) == self.LZO_FAULT
            and bool(self.lzo)
            and self.lzo['length_overlaps_table_pointer']
            and bool(self.lzo['table_pointer_before'])
            and self.lzo['table_pointer_after'] == 0
        )


class TargetCapture(Capture):
    """Every source written as each target and compression, once fresh and twice reused."""

    # (width, height, levels, alpha, patterned, threshold)
    VARIANTS = [
        (2, 2, 1, True, False, 80),  # below 64 bytes: every target skips compression
        (4, 4, 1, True, False, 80),  # exactly 64 bytes: compression is attempted
        (8, 8, 1, True, False, 80),  # a small LZO writer failure
        (7, 5, 3, True, True, 80),  # NPOT, unique supplied levels, COPY fallback
        (8, 8, 4, False, False, 100),  # BGRX, mixed COPY/compressed levels
        (8, 8, 1, True, False, 0),  # threshold cannot prevent the LZO call/fault
        (256, 65, 1, True, False, 80),  # a multi-block LZ4 frame
    ]
    TARGETS = [(0, 'EnfusionDDS'), (2, 'EnfusionDDS_LZ0'), (3, 'EnfusionDDS_LZ4')]
    COMPRESSIONS = ['Copy', 'Fastest', 'Medium', 'Best']

    @staticmethod
    def planted_levels(width: int, height: int, count: int, alpha: bool, patterned: bool) -> list[dict]:
        """Each mip has its own planted pixels; this capture tests storage, not mip filtering."""
        levels = []
        for level in range(count):
            pixels = bytes(
                value
                for y in range(height)
                for x in range(width)
                for value in (
                    (x * 67 + y * 13 + level) % 256 if patterned else 3 + level,
                    (y * 79 + x * 19 + level) % 256 if patterned else 5 + level,
                    (x * 37 + y * 11 + level) % 256 if patterned else 7 + level,
                    (x * 47 + y * 59) % 256 if alpha and patterned else 255,
                )
            )
            levels.append(dict(width=width, height=height, bgra=pixels.hex()))
            width, height = max(width // 2, 1), max(height // 2, 1)

        return levels

    def capture(self) -> dict:
        """Every case; complete files are stored once, by digest, and rows refer to them."""
        rows = []
        sources = []
        files = {}
        for variant in self.VARIANTS:
            width, height, count, alpha, patterned, threshold = variant
            levels = self.planted_levels(width, height, count, alpha, patterned)
            source_index = len(sources)
            sources.append(dict(alpha=alpha, levels=levels))

            for target, name in self.TARGETS:
                for compression, mode in enumerate(self.COMPRESSIONS):
                    arguments = (levels, target, compression, threshold, alpha)
                    try:
                        result = TargetOracle(self.executable).write(*arguments)
                    except Exception as error:
                        raise RuntimeError(f'Capture failed: {variant}, {name}, {mode}') from error

                    reused = TargetOracle(self.executable)
                    for _ in range(2):
                        assert reused.write(*arguments) == result

                    if result['status'] == 'written':
                        encoded = result.pop('edds')
                        digest = hashlib.sha256(bytes.fromhex(encoded)).hexdigest()
                        files[digest] = encoded
                        result['edds_sha256'] = digest

                    rows.append(
                        dict(target=name, target_enum=target, compression=mode, threshold=threshold, source=source_index, result=result)
                    )

        print(f'Captured {len(rows)} target/compression cases (fresh plus two reused executions each).')
        return dict(
            workbench='1.29.163709',
            executable_sha256=self.executable.fingerprint,
            method='Unchanged metadata target dispatch and container writer in Unicorn; memory stream IO',
            limitation='LZO codec bytes captured before an actual writer fault are NOT complete EDDS files',
            sources=sources,
            files=files,
            rows=rows,
        )


if __name__ == '__main__':
    sys.exit(TargetCapture.main(sys.argv[1:], __doc__))
