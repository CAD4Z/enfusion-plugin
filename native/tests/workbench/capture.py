"""Run only the installed texture importer in a CPU emulator; no Windows entrypoint/UI."""
import hashlib
import json
import math
import pathlib
import struct
import sys
import pefile

if len(sys.argv) != 3:
    raise SystemExit('usage: capture.py WORKBENCH_EXE OUTPUT_JSON')
EXE = pathlib.Path(sys.argv[1])
fingerprint = hashlib.sha256(EXE.read_bytes()).hexdigest()
if fingerprint != '74b74d1abe1f7f91e989d33dbfdcf97f9517a3a7df4dcb808c38991fd1df5cbe':
    raise SystemExit('Unsupported Workbench binary; the oracle addresses must be re-established.')
pefile.MAX_IMPORT_SYMBOLS = 16384
pe = pefile.PE(str(EXE))
base = pe.OPTIONAL_HEADER.ImageBase
imports = {i.address: i.name.decode() if i.name else str(i.ordinal)
           for e in pe.DIRECTORY_ENTRY_IMPORT for i in e.imports}
from unicorn import Uc, UC_ARCH_X86, UC_MODE_64, UC_HOOK_CODE, UC_HOOK_MEM_INVALID
from unicorn.x86_const import *

class Oracle:
    def __init__(self):
        self.uc = Uc(UC_ARCH_X86, UC_MODE_64)
        self.uc.mem_map(base, (pe.OPTIONAL_HEADER.SizeOfImage + 4095) & -4096)
        for s in pe.sections:
            self.uc.mem_write(base + s.VirtualAddress, s.get_data())
        self.uc.mem_map(0x20000000, 0x1000000)
        self.uc.mem_map(0x30000000, 0x1000000)
        self.uc.mem_map(0x40000000, 0x100000)
        self.heap = 0x30000000
        self.allocations = {}
        self.stubs = {}
        self.images = {}
        self.trace = []
        self.sentinel = 0x400ffff0
        self.uc.mem_write(self.sentinel, b'\xc3')
        for addr, name in imports.items():
            self.w64(addr, self.stub(lambda n=name: self.import_call(n)))
        self.hook(0x14051f0a0, lambda: 0)  # profiling only
        self.hook(0x14051efc0, lambda: 0)  # profiling only
        self.hook(0x140075190, lambda: 0)  # logger
        self.hook(0x14180cb68, lambda: self.alloc(self.arg(0)))  # operator new
        self.hook(0x14180cb60, lambda: 0)  # sized operator delete
        self.hook(0x14189f80c, self.copy)
        self.hook(0x14042c7c0, lambda: self.image(self.arg(0)))  # raw-image ctor
        self.hook(0x14042c840, lambda: 0)  # raw-image dtor
        self.hook(0x1402b2000, self.property_index)
        self.hook(0x1402dd810, lambda: self.registry)
        self.vtable = self.alloc(0xb0)
        for slot in range(0, 0xb0, 8):
            self.w64(self.vtable + slot, self.stub(lambda s=slot: self.virtual(s)))
        self.registry = self.alloc(0x100)
        context = self.alloc(0x100)
        self.w64(self.registry + 0x68, context)
        self.uc.hook_add(UC_HOOK_CODE, self.on_stub, begin=0x40000000, end=0x400fffef)
        self.uc.hook_add(UC_HOOK_MEM_INVALID, self.invalid)
        self.uc.reg_write(UC_X86_REG_MXCSR, 0x1f80)

    def alloc(self, size):
        if size > 0x400000: raise RuntimeError('oversized oracle allocation '+str(size))
        address = self.heap
        self.heap += (size + 31) & -16
        self.allocations[address] = size
        if self.heap > 0x30ffffff: raise RuntimeError('oracle heap exhausted')
        return address

    def w64(self, address, value): self.uc.mem_write(address, struct.pack('<Q', value))
    def r64(self, address): return struct.unpack('<Q', self.uc.mem_read(address, 8))[0]
    def arg(self, index):
        if index < 4: return self.uc.reg_read([UC_X86_REG_RCX, UC_X86_REG_RDX, UC_X86_REG_R8, UC_X86_REG_R9][index])
        return self.r64(self.uc.reg_read(UC_X86_REG_RSP) + 8 + index*8)

    def stub(self, fn):
        address = 0x40000000 + len(self.stubs)*16
        self.uc.mem_write(address, b'\xc3')
        self.stubs[address] = fn
        return address

    def on_stub(self, uc, address, size, extra):
        result = self.stubs[address]()
        uc.reg_write(UC_X86_REG_RAX, (result or 0) & 0xffffffffffffffff)

    def hook(self, address, fn):
        def call(uc, current, size, extra):
            result = fn()
            stack = uc.reg_read(UC_X86_REG_RSP)
            uc.reg_write(UC_X86_REG_RAX, (result or 0) & 0xffffffffffffffff)
            uc.reg_write(UC_X86_REG_RIP, self.r64(stack))
            uc.reg_write(UC_X86_REG_RSP, stack+8)
        self.uc.hook_add(UC_HOOK_CODE, call, begin=address, end=address)

    def copy(self):
        dest, source, size = [self.arg(i) for i in range(3)]
        self.uc.mem_write(dest, bytes(self.uc.mem_read(source, size)))
        return dest

    def import_call(self, name):
        if name in ['floor','ceil','sqrt','sin','cos','pow','exp','log','acos','atan2']:
            values = [struct.unpack('<d',struct.pack('<Q',self.uc.reg_read(reg)&0xffffffffffffffff))[0] for reg in [UC_X86_REG_XMM0,UC_X86_REG_XMM1]]
            result = getattr(math,name)(*values[:2 if name in ['pow','atan2'] else 1])
            self.uc.reg_write(UC_X86_REG_XMM0, struct.unpack('<Q',struct.pack('<d',result))[0])
            return 0
        if name in ['floorf','ceilf','sqrtf','sinf','cosf','powf','expf','logf','acosf','atan2f']:
            values = [struct.unpack('<f',struct.pack('<I',self.uc.reg_read(reg)&0xffffffff))[0] for reg in [UC_X86_REG_XMM0,UC_X86_REG_XMM1]]
            fn = getattr(math, name[:-1])
            result = fn(*values[:2 if name in ['powf','atan2f'] else 1])
            self.uc.reg_write(UC_X86_REG_XMM0, struct.unpack('<I',struct.pack('<f',result))[0])
            return 0
        if name in ['memcpy','memmove']: return self.copy()
        if name == 'memset':
            self.uc.mem_write(self.arg(0), bytes([self.arg(1)&255])*self.arg(2)); return self.arg(0)
        if name in ['malloc','_aligned_malloc']: return self.alloc(self.arg(0))
        if name in ['free','_aligned_free']: return 0
        if name == 'realloc':
            old, size = self.arg(0), self.arg(1)
            new = self.alloc(size)
            if old: self.uc.mem_write(new, bytes(self.uc.mem_read(old,min(size,self.allocations[old]))))
            return new
        raise RuntimeError('unhandled import '+name+' from '+hex(self.r64(self.uc.reg_read(UC_X86_REG_RSP))))

    def invalid(self, uc, access, address, size, value, extra):
        print('UNMAPPED', hex(address), 'RIP', hex(uc.reg_read(UC_X86_REG_RIP)))
        stack = uc.reg_read(UC_X86_REG_RSP)
        print('STACK', [hex(self.r64(stack+i*8)) for i in range(8)])
        return False

    def image(self, address=None, width=0, height=0, fmt=87, pixels=None, count=1):
        address = address or self.alloc(0x80)
        self.w64(address, self.vtable)
        levels = []
        w,h = width,height
        for level in range(count):
            size = w*h*4
            buf = self.alloc(size)
            if pixels is not None and level == 0: self.uc.mem_write(buf, pixels)
            levels.append((w,h,buf,size))
            w,h = max(w//2,1), max(h//2,1)
        self.images[address] = {'width':width,'height':height,'format':fmt,'levels':levels}
        return address

    def virtual(self, slot):
        address = self.arg(0)
        img = self.images[address]
        if slot == 0x68: return img['format']
        if slot == 0x70: return img['width']
        if slot == 0x78: return img['height']
        if slot in [0x80,0x88]: return 1
        if slot == 0x90: return len(img['levels'])
        if slot == 0xa0: return 0
        if slot == 0x40:
            width,height,count,fmt = self.arg(1),self.arg(2),self.arg(4)&0xffffffff,self.arg(5)&0xffffffff
            self.image(address,width,height,fmt,count=count)
            return 1
        if slot == 0x50:
            level = self.arg(2)
            w,h,p,size = img['levels'][level]
            output = self.arg(3)
            self.uc.mem_write(output, struct.pack('<I4xQ',w*4,p))
            return 1
        if slot == 0x58: return 1
        raise RuntimeError('unhandled image method '+hex(slot))

    def property_index(self):
        return self.arg(1) if self.arg(0) == self.meta and self.arg(1) in self.present else -1

    def convert(self, width,height,pixels,mip_function=0,tiled=True,mip_filter=0,normalize=False,remove=0,generate=True,fmt=87,swizzling=0,conversion=0,quality=1.0,cubemap=False):
        # Verified property-name registration IDs are replaced with stable local IDs.
        values = {0x2321af0:remove,0x2321af4:conversion,0x2321afc:swizzling,0x2321b00:0,0x2321b04:int(generate),
                  0x2321b08:int(normalize),0x2321b0c:mip_function,0x2321b10:int(tiled),0x2321b14:mip_filter,0x2321b1c:int(cubemap),0x2321af8:struct.unpack("<I",struct.pack("<f",quality))[0]}
        self.meta = self.alloc(0x80)
        entries = self.alloc(len(values)*16)
        self.w64(self.meta+0x40,entries)
        self.present = set(range(len(values)))
        for index,(rva,value) in enumerate(values.items()):
            self.uc.mem_write(base+rva,struct.pack('<I',index))
            p = self.alloc(8)
            self.w64(p,value)
            self.w64(entries+index*16+8,p)
        source = self.image(width=width,height=height,pixels=pixels,fmt=fmt)
        output = self.image()
        stack = 0x20ffff08
        self.w64(stack,self.sentinel)
        self.w64(stack+0x28,0)
        self.uc.reg_write(UC_X86_REG_RSP,stack)
        for reg,value in zip([UC_X86_REG_RCX,UC_X86_REG_RDX,UC_X86_REG_R8,UC_X86_REG_R9],[0,source,output,self.meta]): self.uc.reg_write(reg,value)
        try: self.uc.emu_start(0x140fcc0d0,self.sentinel,count=10000000)
        except Exception:
            print('AT',hex(self.uc.reg_read(UC_X86_REG_RIP)))
            raise
        if self.uc.reg_read(UC_X86_REG_RIP) != self.sentinel: raise RuntimeError('instruction limit')
        if not self.uc.reg_read(UC_X86_REG_RAX)&255: raise RuntimeError('importer refused fixture')
        self.output_format = self.images[output]['format']
        return [{'width':w,'height':h,'bgra':bytes(self.uc.mem_read(p,n)).hex()} for w,h,p,n in self.images[output]['levels']]

def pixels_of(width, height, alpha):
    return bytes(v for y in range(height) for x in range(width) for v in
        [(x*67+y*13)%256, (y*79+x*19)%256,
         255 if x == 0 else 0 if x == width-1 else (x*37+y*11)%256,
         (x*47+y*59)%256 if alpha else 255])


if __name__ == '__main__':
    rows = []
    for alpha, dimensions, functions in [
        (True, [(8,8),(7,5),(1,7),(7,1),(2,2),(1,1)], [0,2]),
        (False, [(8,8),(7,5)], [2]),
    ]:
        for width, height in dimensions:
            pixels = pixels_of(width, height, alpha)
            for function in functions:
                for tiled in [False, True]:
                    for filt in [0,2]:
                        args = (width, height, pixels, function, tiled, filt)
                        levels = Oracle().convert(*args, fmt=87 if alpha else 88)
                        reused = Oracle()
                        for repeat in range(2):
                            assert reused.convert(*args, fmt=87 if alpha else 88) == levels
                        row = dict(width=width, height=height, source_bgra=pixels.hex(),
                                   mip_function=function, tiled=tiled, mip_filter=filt, levels=levels)
                        if not alpha:
                            row['alpha'] = False
                        rows.append(row)
                        print(width, height, function, tiled, filt, 'repeatable', flush=True)
    pathlib.Path(sys.argv[2]).write_text(json.dumps(dict(
        workbench_sha256=fingerprint, rows=rows,
        repeatability='Each case captured in a fresh emulator and twice in a reused emulator; all three outputs identical.'
    ), indent=2) + '\n')
