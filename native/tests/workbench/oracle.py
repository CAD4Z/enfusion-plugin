"""The installed Workbench texture importer, run in a CPU emulator instead of Workbench itself.

Only the importer's machine code executes: no Windows entry point, window or process. The emulator
maps the executable's image, answers the C runtime imports the importer calls, and stands an image
object of its own behind the importer's virtual calls. Every address here belongs to one build of
workbenchApp.exe, and `WorkbenchExecutable` refuses to load any other.
"""

import argparse
import hashlib
import json
import math
import pathlib
import struct

import pefile
from unicorn import UC_ARCH_X86, UC_HOOK_CODE, UC_HOOK_MEM_INVALID, UC_MODE_64, Uc
from unicorn.x86_const import (
    UC_X86_REG_MXCSR,
    UC_X86_REG_R8,
    UC_X86_REG_R9,
    UC_X86_REG_RAX,
    UC_X86_REG_RCX,
    UC_X86_REG_RDX,
    UC_X86_REG_RIP,
    UC_X86_REG_RSP,
    UC_X86_REG_XMM0,
    UC_X86_REG_XMM1,
)

# The registers of the first four integer arguments in the Windows x64 calling convention.
ARGUMENT_REGISTERS = (UC_X86_REG_RCX, UC_X86_REG_RDX, UC_X86_REG_R8, UC_X86_REG_R9)


class WorkbenchExecutable:
    """The one workbenchApp.exe whose addresses the oracles were established in, parsed once."""

    SHA256 = '74b74d1abe1f7f91e989d33dbfdcf97f9517a3a7df4dcb808c38991fd1df5cbe'

    def __init__(self, path: pathlib.Path):
        self.fingerprint = hashlib.sha256(path.read_bytes()).hexdigest()
        if self.fingerprint != self.SHA256:
            raise SystemExit('Unsupported Workbench binary; the oracle addresses must be re-established.')

        # The executable imports more symbols than pefile reads by default.
        pefile.MAX_IMPORT_SYMBOLS = 16384
        self.pe = pefile.PE(str(path))
        self.base = self.pe.OPTIONAL_HEADER.ImageBase
        self.imports = {
            symbol.address: symbol.name.decode() if symbol.name else str(symbol.ordinal)
            for entry in self.pe.DIRECTORY_ENTRY_IMPORT
            for symbol in entry.imports
        }


class Capture:
    """A capture script: the installed executable in, one JSON file of evidence out.

    A subclass says what it captures; the command line and the file are the same for all of them.
    """

    def __init__(self, executable: WorkbenchExecutable):
        self.executable = executable

    def capture(self) -> dict:
        """Everything the file records."""
        raise NotImplementedError

    def write(self, output: pathlib.Path) -> None:
        """Runs the capture into `output`, as UTF-8 JSON with LF line ends on every platform."""
        output.write_text(json.dumps(self.capture(), indent=2) + '\n', encoding='utf-8', newline='\n')

    @classmethod
    def main(cls, arguments: list[str], description: str) -> int:
        """The command line: WORKBENCH_EXE OUTPUT_JSON."""
        parser = argparse.ArgumentParser(description=description)
        parser.add_argument('workbench', type=pathlib.Path, help='the installed workbenchApp.exe')
        parser.add_argument('output', type=pathlib.Path, help='the capture to write')
        options = parser.parse_args(arguments)

        cls(WorkbenchExecutable(options.workbench)).write(options.output)
        return 0


class SourcePattern:
    """The owned BGRA test pattern the mip and swizzle captures feed the importer."""

    @staticmethod
    def bgra(width: int, height: int, alpha: bool) -> bytes:
        """Distinct channels, a full first and empty last blue column, varying or opaque alpha."""
        return bytes(
            value
            for y in range(height)
            for x in range(width)
            for value in [
                (x * 67 + y * 13) % 256,
                (y * 79 + x * 19) % 256,
                255 if x == 0 else 0 if x == width - 1 else (x * 37 + y * 11) % 256,
                (x * 47 + y * 59) % 256 if alpha else 255,
            ]
        )


class Oracle:
    """The importer in an emulator, with its imports, helpers and image object answered in Python.

    A stub is a lone `ret` in the stub area; executing it runs the Python function registered for
    it, whose result becomes RAX. A hook replaces an internal function at its address the same way
    and returns to its caller.
    """

    # Where the emulator keeps what the importer does not own.
    STACK = 0x20000000
    STACK_SIZE = 0x1000000
    STACK_TOP = 0x20FFFF08
    HEAP = 0x30000000
    HEAP_SIZE = 0x1000000
    STUBS = 0x40000000
    STUBS_SIZE = 0x100000

    # A `ret` past the last stub: every call made from Python returns to it, and emulation stops.
    SENTINEL = 0x400FFFF0

    # One allocation larger than this means the importer misread the fixture.
    LARGEST_ALLOCATION = 0x400000

    # The importer's texture conversion function and the property-registration IDs of the profile it
    # reads, each given a stable local index before the call.
    CONVERT = 0x140FCC0D0
    PROFILE_PROPERTIES = {
        'remove': 0x2321AF0,
        'conversion': 0x2321AF4,
        'swizzling': 0x2321AFC,
        'zero': 0x2321B00,  # registered between Swizzling and GenerateMips; every capture leaves it 0
        'generate': 0x2321B04,
        'normalize': 0x2321B08,
        'mip_function': 0x2321B0C,
        'tiled': 0x2321B10,
        'mip_filter': 0x2321B14,
        'cubemap': 0x2321B1C,
        'quality': 0x2321AF8,
    }

    # The C runtime math the importer imports, as double and float functions; those in BINARY_MATH
    # take two arguments, the rest one.
    DOUBLE_MATH = {'floor', 'ceil', 'sqrt', 'sin', 'cos', 'pow', 'exp', 'log', 'acos', 'atan2'}
    FLOAT_MATH = {name + 'f' for name in DOUBLE_MATH}
    BINARY_MATH = {'pow', 'atan2', 'powf', 'atan2f'}

    def __init__(self, executable: WorkbenchExecutable):
        self.executable = executable
        self.uc = Uc(UC_ARCH_X86, UC_MODE_64)

        # The executable's image, section by section, at its preferred base.
        self.uc.mem_map(executable.base, (executable.pe.OPTIONAL_HEADER.SizeOfImage + 4095) & -4096)
        for section in executable.pe.sections:
            self.uc.mem_write(executable.base + section.VirtualAddress, section.get_data())

        # The emulator's own memory and bookkeeping.
        self.uc.mem_map(self.STACK, self.STACK_SIZE)
        self.uc.mem_map(self.HEAP, self.HEAP_SIZE)
        self.uc.mem_map(self.STUBS, self.STUBS_SIZE)
        self.heap = self.HEAP
        self.allocations = {}
        self.stubs = {}
        self.images = {}
        self.uc.mem_write(self.SENTINEL, b'\xc3')

        # Every import answers through import_call.
        for address, name in executable.imports.items():
            self.write64(address, self.stub(lambda name=name: self.import_call(name)))

        # Internal functions the emulator provides itself, or that do nothing useful here.
        self.hook(0x14051F0A0, lambda: 0)  # profiling only
        self.hook(0x14051EFC0, lambda: 0)  # profiling only
        self.hook(0x140075190, lambda: 0)  # logger
        self.hook(0x14180CB68, lambda: self.alloc(self.arg(0)))  # operator new
        self.hook(0x14180CB60, lambda: 0)  # sized operator delete
        self.hook(0x14189F80C, self.copy)
        self.hook(0x14042C7C0, lambda: self.image(self.arg(0)))  # raw-image constructor
        self.hook(0x14042C840, lambda: 0)  # raw-image destructor
        self.hook(0x1402B2000, self.property_index)
        self.hook(0x1402DD810, lambda: self.registry)

        # The image object's virtual methods, one stub per slot.
        self.vtable = self.alloc(0xB0)
        for slot in range(0, 0xB0, 8):
            self.write64(self.vtable + slot, self.stub(lambda slot=slot: self.virtual(slot)))

        # The property registry the importer asks for, with an empty context behind it.
        self.registry = self.alloc(0x100)
        context = self.alloc(0x100)
        self.write64(self.registry + 0x68, context)

        self.uc.hook_add(UC_HOOK_CODE, self.on_stub, begin=self.STUBS, end=self.SENTINEL - 1)
        self.uc.hook_add(UC_HOOK_MEM_INVALID, self.invalid)
        self.uc.reg_write(UC_X86_REG_MXCSR, 0x1F80)

    def alloc(self, size: int) -> int:
        """A fresh heap block of `size` bytes; the heap never frees."""
        if size > self.LARGEST_ALLOCATION:
            raise RuntimeError('oversized oracle allocation ' + str(size))

        address = self.heap
        self.heap += (size + 31) & -16
        self.allocations[address] = size
        if self.heap > self.HEAP + self.HEAP_SIZE - 1:
            raise RuntimeError('oracle heap exhausted')

        return address

    def write64(self, address: int, value: int) -> None:
        """Stores an unsigned 64-bit value."""
        self.uc.mem_write(address, struct.pack('<Q', value))

    def read64(self, address: int) -> int:
        """Loads an unsigned 64-bit value."""
        return struct.unpack('<Q', self.uc.mem_read(address, 8))[0]

    def arg(self, index: int) -> int:
        """The integer argument `index` of the call being answered: a register, then the stack."""
        if index < 4:
            return self.uc.reg_read(ARGUMENT_REGISTERS[index])

        return self.read64(self.uc.reg_read(UC_X86_REG_RSP) + 8 + index * 8)

    def xmm_doubles(self) -> list[float]:
        """The double arguments in XMM0 and XMM1."""
        return [
            struct.unpack('<d', struct.pack('<Q', self.uc.reg_read(register) & 0xFFFFFFFFFFFFFFFF))[0]
            for register in (UC_X86_REG_XMM0, UC_X86_REG_XMM1)
        ]

    def xmm_floats(self) -> list[float]:
        """The float arguments in XMM0 and XMM1."""
        return [
            struct.unpack('<f', struct.pack('<I', self.uc.reg_read(register) & 0xFFFFFFFF))[0]
            for register in (UC_X86_REG_XMM0, UC_X86_REG_XMM1)
        ]

    def return_double(self, value: float) -> int:
        """Puts a double result in XMM0; RAX is left zero."""
        self.uc.reg_write(UC_X86_REG_XMM0, struct.unpack('<Q', struct.pack('<d', value))[0])
        return 0

    def return_float(self, value: float) -> int:
        """Puts a float result in XMM0; RAX is left zero."""
        self.uc.reg_write(UC_X86_REG_XMM0, struct.unpack('<I', struct.pack('<f', value))[0])
        return 0

    def stub(self, answer) -> int:
        """A new stub address that runs `answer` when executed."""
        address = self.STUBS + len(self.stubs) * 16
        self.uc.mem_write(address, b'\xc3')
        self.stubs[address] = answer
        return address

    def on_stub(self, uc, address, size, extra) -> None:
        """Runs the stub's answer just before its `ret` executes."""
        result = self.stubs[address]()
        uc.reg_write(UC_X86_REG_RAX, (result or 0) & 0xFFFFFFFFFFFFFFFF)

    def hook(self, address: int, answer) -> None:
        """Replaces the function at `address`: `answer` gives RAX, then it returns to the caller."""

        def call(uc, current, size, extra):
            result = answer()
            stack = uc.reg_read(UC_X86_REG_RSP)
            uc.reg_write(UC_X86_REG_RAX, (result or 0) & 0xFFFFFFFFFFFFFFFF)
            uc.reg_write(UC_X86_REG_RIP, self.read64(stack))
            uc.reg_write(UC_X86_REG_RSP, stack + 8)

        self.uc.hook_add(UC_HOOK_CODE, call, begin=address, end=address)

    def call(self, address: int, arguments: list[int], count: int, stack: dict[int, int] | None = None) -> None:
        """Calls `address` and runs until it returns to the sentinel or `count` instructions pass.

        `arguments` fill the argument registers in order; `stack` maps offsets above the return
        address to further 64-bit slots of the frame.
        """
        self.write64(self.STACK_TOP, self.SENTINEL)
        for offset, value in (stack or {}).items():
            self.write64(self.STACK_TOP + offset, value)

        self.uc.reg_write(UC_X86_REG_RSP, self.STACK_TOP)
        for register, value in zip(ARGUMENT_REGISTERS, arguments):
            self.uc.reg_write(register, value)

        self.uc.emu_start(address, self.SENTINEL, count=count)

    def copy(self) -> int:
        """memcpy: copies the third argument's count of bytes and returns the destination."""
        destination, source, size = [self.arg(index) for index in range(3)]
        self.uc.mem_write(destination, bytes(self.uc.mem_read(source, size)))
        return destination

    def import_call(self, name: str) -> int:
        """Answers one C runtime import by name; an import nobody answers stops the run."""
        if name in self.DOUBLE_MATH:
            values = self.xmm_doubles()
            return self.return_double(getattr(math, name)(*values[: 2 if name in self.BINARY_MATH else 1]))

        if name in self.FLOAT_MATH:
            values = self.xmm_floats()
            return self.return_float(getattr(math, name[:-1])(*values[: 2 if name in self.BINARY_MATH else 1]))

        if name in ['memcpy', 'memmove']:
            return self.copy()

        if name == 'memset':
            self.uc.mem_write(self.arg(0), bytes([self.arg(1) & 255]) * self.arg(2))
            return self.arg(0)

        if name in ['malloc', '_aligned_malloc']:
            return self.alloc(self.arg(0))

        if name in ['free', '_aligned_free']:
            return 0

        if name == 'realloc':
            old, size = self.arg(0), self.arg(1)
            new = self.alloc(size)
            if old:
                self.uc.mem_write(new, bytes(self.uc.mem_read(old, min(size, self.allocations[old]))))
            return new

        raise RuntimeError('unhandled import ' + name + ' from ' + hex(self.read64(self.uc.reg_read(UC_X86_REG_RSP))))

    def invalid(self, uc, access, address, size, value, extra) -> bool:
        """Reports an access outside mapped memory, with the stack, and lets the run fail."""
        print('UNMAPPED', hex(address), 'RIP', hex(uc.reg_read(UC_X86_REG_RIP)))
        stack = uc.reg_read(UC_X86_REG_RSP)
        print('STACK', [hex(self.read64(stack + slot * 8)) for slot in range(8)])
        return False

    def image(self, address=None, width=0, height=0, fmt=87, pixels=None, count=1) -> int:
        """An image of `count` BGRA levels at `address`, or a new one; `pixels` fill level 0."""
        address = address or self.alloc(0x80)
        self.write64(address, self.vtable)

        levels = []
        level_width, level_height = width, height
        for level in range(count):
            size = level_width * level_height * 4
            buffer = self.alloc(size)
            if pixels is not None and level == 0:
                self.uc.mem_write(buffer, pixels)
            levels.append((level_width, level_height, buffer, size))
            level_width, level_height = max(level_width // 2, 1), max(level_height // 2, 1)

        self.images[address] = {'width': width, 'height': height, 'format': fmt, 'levels': levels}
        return address

    def virtual(self, slot: int) -> int:
        """Answers the virtual method at `slot` of the image in the first argument."""
        address = self.arg(0)
        image = self.images[address]

        if slot == 0x68:
            return image['format']
        if slot == 0x70:
            return image['width']
        if slot == 0x78:
            return image['height']
        if slot in [0x80, 0x88]:
            return 1
        if slot == 0x90:
            return len(image['levels'])
        if slot == 0xA0:
            return 0

        # Create: (width, height, _, count, format) rebuilds the image in place.
        if slot == 0x40:
            width, height, count, fmt = self.arg(1), self.arg(2), self.arg(4) & 0xFFFFFFFF, self.arg(5) & 0xFFFFFFFF
            self.image(address, width, height, fmt, count=count)
            return 1

        # Lock: writes the level's row pitch and pixel address to the fourth argument.
        if slot == 0x50:
            level_width, _, pixels, _ = image['levels'][self.arg(2)]
            self.uc.mem_write(self.arg(3), struct.pack('<I4xQ', level_width * 4, pixels))
            return 1

        if slot == 0x58:
            return 1

        raise RuntimeError('unhandled image method ' + hex(slot))

    def property_index(self) -> int:
        """The property's local index when the importer asks the current profile, else -1."""
        return self.arg(1) if self.arg(0) == self.meta and self.arg(1) in self.present else -1

    def write_profile(self, values: dict[int, int]) -> None:
        """A profile object whose properties, keyed by registration ID, hold `values` in order."""
        self.meta = self.alloc(0x80)
        entries = self.alloc(len(values) * 16)
        self.write64(self.meta + 0x40, entries)
        self.present = set(range(len(values)))

        for index, (registration, value) in enumerate(values.items()):
            self.uc.mem_write(self.executable.base + registration, struct.pack('<I', index))
            slot = self.alloc(8)
            self.write64(slot, value)
            self.write64(entries + index * 16 + 8, slot)

    def convert(
        self,
        width,
        height,
        pixels,
        mip_function=0,
        tiled=True,
        mip_filter=0,
        normalize=False,
        remove=0,
        generate=True,
        fmt=87,
        swizzling=0,
        conversion=0,
        quality=1.0,
        cubemap=False,
    ) -> list[dict]:
        """Runs the importer on one source and profile; returns its output levels as hex BGRA."""
        properties = self.PROFILE_PROPERTIES
        self.write_profile(
            {
                properties['remove']: remove,
                properties['conversion']: conversion,
                properties['swizzling']: swizzling,
                properties['zero']: 0,
                properties['generate']: int(generate),
                properties['normalize']: int(normalize),
                properties['mip_function']: mip_function,
                properties['tiled']: int(tiled),
                properties['mip_filter']: mip_filter,
                properties['cubemap']: int(cubemap),
                properties['quality']: struct.unpack('<I', struct.pack('<f', quality))[0],
            }
        )

        source = self.image(width=width, height=height, pixels=pixels, fmt=fmt)
        output = self.image()
        try:
            self.call(self.CONVERT, [0, source, output, self.meta], count=10000000, stack={0x28: 0})
        except Exception:
            print('AT', hex(self.uc.reg_read(UC_X86_REG_RIP)))
            raise

        if self.uc.reg_read(UC_X86_REG_RIP) != self.SENTINEL:
            raise RuntimeError('instruction limit')
        if not self.uc.reg_read(UC_X86_REG_RAX) & 255:
            raise RuntimeError('importer refused fixture')

        self.output_format = self.images[output]['format']
        self.converted_address = output
        return [
            {'width': level_width, 'height': level_height, 'bgra': bytes(self.uc.mem_read(pixels, size)).hex()}
            for level_width, level_height, pixels, size in self.images[output]['levels']
        ]
