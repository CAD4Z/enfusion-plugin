"""Capture installed HDR decoder, float importer, cubemap projection and container writer.
The final BC encoder boundary is recorded; these are not Workbench BC6H blocks.
"""
from capture import *
class HdrOracle(Oracle):
    def __init__(self):
        super().__init__()
        self.hook(0x140f4cdb0, self.capture_encoder)
        self.hook(0x14042fe90, lambda: self.virtual(0x50))
        self.hook(0x14042ff00, lambda: 1)
        self.hook(0x14180cbc0, lambda: 0) # stack probing only
        self.encoder = None
        self.logs = []

    def import_call(self,name):
        if name=='GetProcessHeap':return 1
        if name=='HeapAlloc':return self.alloc(self.arg(2))
        if name=='HeapFree':return 1
        if name=='HeapSize':return self.allocations[self.arg(2)]
        if name=='ldexp':
            v=struct.unpack('<d',struct.pack('<Q',self.uc.reg_read(UC_X86_REG_XMM0)&0xffffffffffffffff))[0]
            n=self.arg(1)&0xffffffff;n=n if n<0x80000000 else n-0x100000000
            self.uc.reg_write(UC_X86_REG_XMM0,struct.unpack('<Q',struct.pack('<d',math.ldexp(v,n)))[0]);return 0
        if name in ['strcmp','strncmp']:
            limit=self.arg(2) if name=='strncmp' else 4096
            def read(p):
                out=bytearray()
                for i in range(limit):
                    v=bytes(self.uc.mem_read(p+i,1))[0]
                    if not v:break
                    out.append(v)
                return bytes(out)
            a,b=read(self.arg(0)),read(self.arg(1))
            return (a>b)-(a<b)
        if name=='strtol':
            import re
            a=self.arg(0)
            s=bytes(self.uc.mem_read(a,200)).split(b'\0')[0]
            match=re.match(rb'\s*[+-]?\d+',s)
            n=len(match[0]) if match else 0
            if self.arg(1):self.w64(self.arg(1),a+n)
            return int(match[0]) if match else 0
        if name == '_hypotf':
            values=[struct.unpack('<f',struct.pack('<I',self.uc.reg_read(reg)&0xffffffff))[0] for reg in [UC_X86_REG_XMM0,UC_X86_REG_XMM1]]
            self.uc.reg_write(UC_X86_REG_XMM0,struct.unpack('<I',struct.pack('<f',math.hypot(*values)))[0])
            return 0
        return super().import_call(name)

    def decode_hdr(self, data):
        buf=self.alloc(len(data));self.uc.mem_write(buf,data)
        ctx=self.alloc(0xe0)
        for offset,value in [(0xb8,buf),(0xc0,buf+len(data)),(0xc8,buf),(0xd0,buf+len(data))]:self.w64(ctx+offset,value)
        dims=self.alloc(32)
        stack=0x20ffff08
        self.w64(stack,self.sentinel);self.w64(stack+0x28,3);self.w64(stack+0x30,dims+16)
        self.uc.reg_write(UC_X86_REG_RSP,stack)
        for reg,val in zip([UC_X86_REG_RCX,UC_X86_REG_RDX,UC_X86_REG_R8,UC_X86_REG_R9],[ctx,dims,dims+4,dims+8]):self.uc.reg_write(reg,val)
        self.uc.emu_start(0x140d88630,self.sentinel,count=1000000)
        out=self.uc.reg_read(UC_X86_REG_RAX)
        if not out:return None
        w,h,c=struct.unpack('<3I',self.uc.mem_read(dims,12))
        return w,h,c,list(struct.unpack('<'+'f'*(w*h*c),self.uc.mem_read(out,w*h*c*4)))

    def convert(self, *args, **kwargs):
        levels = super().convert(*args, **kwargs)
        if self.encoder is None:
            # Conversion=None never reaches the encoder: the importer's own output is serialized.
            self.output_address = self.converted_address
        return levels

    def write_container(self):
        out=self.alloc(64);vtable=self.alloc(64)
        self.w64(out,vtable)
        self.w64(vtable+8,self.stub(lambda:1))
        parts=[]
        def write():
            n=self.arg(2);parts.append(bytes(self.uc.mem_read(self.arg(1),n)));return n
        self.w64(vtable+0x28,self.stub(write))
        src=self.output_address
        params=self.alloc(32);self.uc.mem_write(params,struct.pack('<4I',0,0,0,self.images[src]['count']))
        stack=0x20ffff08;self.w64(stack,self.sentinel)
        self.uc.reg_write(UC_X86_REG_RSP,stack)
        for reg,val in zip([UC_X86_REG_RCX,UC_X86_REG_RDX,UC_X86_REG_R8],[out,src,params]):self.uc.reg_write(reg,val)
        self.uc.emu_start(0x140429a30,self.sentinel,count=10000000)
        return b''.join(parts)

    def log(self):
        p = self.arg(2)
        self.logs.append(bytes(self.uc.mem_read(p, 400)).split(b'\0')[0].decode(errors='replace'))
        return 0

    def image(self, address=None, width=0, height=0, fmt=87, pixels=None, count=1, faces=1):
        address = address or self.alloc(0x80)
        self.w64(address, self.vtable)
        self.uc.mem_write(address + 8, struct.pack('<IHHHBB', fmt, width, height, 1, count, faces))
        levels = []
        for face in range(faces):
            w,h = width,height
            for level in range(count):
                size = ((w+3)//4)*((h+3)//4)*16 if fmt == 95 else w*h*(16 if fmt == 2 else 4)
                buf = self.alloc(size)
                if pixels is not None and level == 0: self.uc.mem_write(buf, pixels)
                levels.append((w,h,buf,size))
                w,h = max(w//2,1),max(h//2,1)
        self.images[address] = dict(width=width,height=height,format=fmt,levels=levels,count=count,faces=faces)
        return address

    def virtual(self, slot):
        address = self.arg(0)
        img = self.images[address]
        if slot == 0x80: return 1
        if slot == 0x88: return img['faces']
        if slot == 0x90: return img['count']
        if slot == 0xa0: return int(img['faces']==6)
        if slot == 0x40:
            # (width, height, array size, mips, format, flags, ...): flag 0x1000 makes a six-face cube,
            # whatever array size it is created with; the importer's None cube output passes size 1.
            faces = 6 if self.arg(6)&0x1000 else self.arg(3)&0xffffffff
            self.image(address,self.arg(1),self.arg(2),self.arg(5)&0xffffffff,
                       count=self.arg(4)&0xffffffff,faces=faces)
            return 1
        if slot == 0x50:
            face,level = self.arg(1),self.arg(2)
            w,h,p,size = img['levels'][face*img['count']+level]
            pitch = ((w+3)//4)*16 if img['format']==95 else w*(16 if img['format']==2 else 4)
            self.uc.mem_write(self.arg(3),struct.pack('<I4xQ',pitch,p))
            return 1
        return super().virtual(slot)

    def capture_encoder(self):
        src,out,params = [self.arg(i) for i in range(3)]
        self.encoder = dict(format=struct.unpack('<I',self.uc.mem_read(params,4))[0],
                            quality=struct.unpack('<f',self.uc.mem_read(params+24,4))[0])
        img = self.images[src]
        self.image(out,img['width'],img['height'],img['format'],count=img['count'],faces=img['faces'])
        for a,b in zip(img['levels'],self.images[out]['levels']):
            self.uc.mem_write(b[2],bytes(self.uc.mem_read(a[2],a[3])))
        self.captured = self.images[out]
        self.output_address=out
        return 0


class EncoderOptions(HdrOracle):
    """Runs the installed BC6H encoder itself, unintercepted, until it starts its worker threads.

    The encoder hands its codec named options as strings before any block is encoded; those strings
    are recorded. Its thread pool cannot run in a single-threaded emulator, so the run stops there.
    """
    class Started(Exception):
        pass

    def __init__(self):
        self.strings = []
        super().__init__()

    def hook(self, address, fn):
        if address != 0x140f4cdb0:
            super().hook(address, fn)

    def import_call(self, name):
        if name == '__stdio_common_vsprintf':
            text = self.vsprintf(bytes(self.uc.mem_read(self.arg(3), 200)).split(b'\0')[0].decode(), self.arg(5))
            self.strings.append(text)
            data = text.encode() + b'\0'
            if self.arg(1) and self.arg(2) >= len(data):
                self.uc.mem_write(self.arg(1), data)
            return len(text)
        if name == '_controlfp_s':
            if self.arg(0):
                self.uc.mem_write(self.arg(0), struct.pack('<I', 0x9001f))
            return 0
        if name in ['_Cnd_init', '_Mtx_init']:
            self.w64(self.arg(0), self.alloc(64))
            return 0
        if name in ['_Mtx_lock', '_Mtx_unlock']:
            return 0
        if name == '_Thrd_start':
            raise EncoderOptions.Started()
        return super().import_call(name)

    def vsprintf(self, fmt, va):
        import re
        out, index = '', 0
        for piece in re.split(r'(%[-+ #0]*\d*(?:\.\d+)?[dusf])', fmt):
            if not piece.startswith('%'):
                out += piece
                continue
            raw = self.r64(va + index * 8)
            index += 1
            if piece[-1] == 'f':
                out += piece % struct.unpack('<d', struct.pack('<Q', raw))[0]
            elif piece[-1] == 's':
                out += bytes(self.uc.mem_read(raw, 200)).split(b'\0')[0].decode()
            else:
                value = raw & 0xffffffff
                out += piece % (value - (1 << 32) if piece[-1] == 'd' and value >= 0x80000000 else value)
        return out


def rgbe_source(w, h, exponent=lambda x, y: 132):
    """An owned flat Radiance file, and the RGBA32F samples the installed decoder reads from it."""
    pixels = bytes(v for y in range(h) for x in range(w)
                   for v in [128 | (x * 37 + y * 11) % 128, 128 | (y * 29 + x * 7) % 128, 128 | (x * 5 + y * 3) % 128, exponent(x, y)])
    source = f'#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y {h} +X {w}\n'.encode() + pixels
    decoded = HdrOracle().decode_hdr(source)
    rgb = decoded[3]
    return source, b''.join(struct.pack('<4f', *rgb[i:i + 3], 1) for i in range(0, len(rgb), 3))


def accepted(w, h, pixels, **args):
    """Whether the installed importer accepts this float source and profile, and its levels if it does."""
    try:
        return HdrOracle().convert(w, h, pixels, fmt=2, **args)
    except RuntimeError as error:
        if 'importer refused fixture' not in str(error):
            raise
        return None


if __name__ == '__main__':
    rows=[]
    decoder=[]
    for rle in [False,True]:
        w,h=16,8
        pixels=bytes(v for y in range(h) for x in range(w) for v in [64+x*4,32+y*8,16+x+y,132])
        payload=pixels
        if rle:
            payload=b''.join(bytes([2,2,0,w])+b''.join(bytes([w])+pixels[(y*w)*4+c:(y*w+w)*4:4] for c in range(4)) for y in range(h))
        for signature in ['RADIANCE','RGBE']:
            hdr=f'#?{signature}\nFORMAT=32-bit_rle_rgbe\n\n-Y {h} +X {w}\n'.encode()+payload
            decoded=HdrOracle().decode_hdr(hdr)
            assert decoded==HdrOracle().decode_hdr(hdr)
            decoder.append(dict(source=hdr.hex(),width=w,height=h,rgb=decoded[3]))
    # HDRCompression (7) at three qualities with the first two filter/tiling pairs; then the other
    # two pairs, and Conversion=None (0), whose RGBA32F output the importer itself produces.
    cases=[(7,cube,generate,quality,filt,tiled) for cube in [False,True] for generate in [False,True]
           for quality in [0,0.403,1] for filt,tiled in [(0,True),(2,False)]]
    cases+=[(7,cube,True,1,filt,tiled) for cube in [False,True] for filt,tiled in [(0,False),(2,True)]]
    cases+=[(0,cube,generate,1,filt,tiled) for cube in [False,True] for generate in [False,True]
            for filt,tiled in ([(0,True),(2,False),(0,False),(2,True)] if generate else [(0,True)])]
    for conversion,cube,generate,quality,filt,tiled in cases:
        o=HdrOracle()
        w,h,rgb=16,8,decoder[0]['rgb']
        pixels=b''.join(struct.pack('<4f',*rgb[i:i+3],1) for i in range(0,len(rgb),3))
        args=dict(fmt=2,conversion=conversion,cubemap=cube,generate=generate,quality=quality,mip_filter=filt,tiled=tiled)
        levels=o.convert(w,h,pixels,**args)
        reused=HdrOracle()
        for repeat in range(2):assert reused.convert(w,h,pixels,**args)==levels
        container=o.write_container()
        rows.append(dict(width=w,height=h,conversion=conversion,cubemap=cube,generate=generate,quality=quality,filter=filt,tiled=tiled,
            encoder=o.encoder,output_format=o.output_format,source_rgba=pixels.hex(),
            levels=[dict(width=l['width'],height=l['height'],rgba=l['bgra']) for l in levels],
            float_container=container.hex()))
        print(conversion,cube,generate,quality,filt,tiled,flush=True)
    containers=[]
    for faces in [1,6]:
        o=HdrOracle()
        o.output_address=o.image(width=4,height=4,fmt=95,count=3,faces=faces)
        containers.append(dict(faces=faces,bc6_container=o.write_container().hex()))
    # Which source sizes the importer accepts: HDRCompression needs power-of-two sides of at least
    # four; None takes any size, so its levels are recorded too.
    dimensions=[]
    for conversion in [7,0]:
        for w,h in [(4,4),(8,4),(4,8),(16,8),(12,8),(6,6),(3,5),(2,2),(1,1)]:
            hdr,pixels=rgbe_source(w,h)
            levels=accepted(w,h,pixels,conversion=conversion,generate=True)
            row=dict(conversion=conversion,width=w,height=h,source=hdr.hex(),accepted=levels is not None)
            if levels is not None and conversion==0:
                row.update(levels=[dict(width=l['width'],height=l['height'],rgba=l['bgra']) for l in levels])
            dimensions.append(row)
            print('dimensions',conversion,w,h,row['accepted'],flush=True)
    panoramas=[]
    for w,h in [(16,8),(32,16),(24,12),(8,4),(32,8),(16,16)]:
        hdr,pixels=rgbe_source(w,h)
        levels=accepted(w,h,pixels,conversion=7,cubemap=True,generate=False)
        panoramas.append(dict(width=w,height=h,source=hdr.hex(),accepted=levels is not None))
        print('panorama',w,h,levels is not None,flush=True)
    # Radiance above the half-float range (the two left columns, exponent 146: 2^17 and more)
    # reaches the encoder unchanged; it is the encoder that saturates.
    hdr,big=rgbe_source(16,8,lambda x,y:146 if x<2 else 132)
    levels=HdrOracle().convert(16,8,big,fmt=2,conversion=7,generate=False)
    reached=struct.unpack('<'+'f'*(16*8*4),bytes.fromhex(levels[0]['bgra']))
    above_half=dict(source=hdr.hex(),source_max=max(struct.unpack('<'+'f'*(16*8*4),big)),encoder_input_max=max(reached))
    print('above half',above_half['source_max'],above_half['encoder_input_max'],flush=True)
    # The options the installed encoder hands its codec, as strings, for ConversionQuality 0.403.
    o=EncoderOptions()
    try:
        o.convert(4,4,rgbe_source(4,4)[1],fmt=2,conversion=7,generate=False,quality=0.403)
        raise RuntimeError('the encoder returned without starting its workers')
    except EncoderOptions.Started:
        pass
    encoder_options=dict(zip(o.strings[::2],o.strings[1::2]))
    print('encoder options',encoder_options,flush=True)
    pathlib.Path(sys.argv[2]).write_bytes((json.dumps(dict(workbench_sha256=fingerprint,decoder=decoder,rows=rows,containers=containers,
        dimensions=dimensions,panoramas=panoramas,above_half=above_half,encoder_options=encoder_options,
        evidence='Installed Radiance decoder and importer execute unchanged. HDRCompression encoder format and quality are intercepted; '
                 'None output is the importer\'s own. Float containers are serialized by the installed writer. Each importer case once '
                 'fresh and twice reused. encoder_options are the strings the unintercepted encoder passes to its codec before its '
                 'worker threads start.'),indent=2)+'\n').encode())
