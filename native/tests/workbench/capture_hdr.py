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
            self.image(address,self.arg(1),self.arg(2),self.arg(5)&0xffffffff,
                       count=self.arg(4)&0xffffffff,faces=self.arg(3)&0xffffffff)
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
    for cube in [False,True]:
        for generate in [False,True]:
            for quality in [0,0.403,1]:
                for filt,tiled in [(0,True),(2,False)]:
                    o=HdrOracle()
                    w,h,rgb=16,8,decoder[0]['rgb']
                    pixels=b''.join(struct.pack('<4f',*rgb[i:i+3],1) for i in range(0,len(rgb),3))
                    args=dict(fmt=2,conversion=7,cubemap=cube,generate=generate,quality=quality,mip_filter=filt,tiled=tiled)
                    levels=o.convert(w,h,pixels,**args)
                    reused=HdrOracle()
                    for repeat in range(2):assert reused.convert(w,h,pixels,**args)==levels
                    container=o.write_container()
                    rows.append(dict(width=w,height=h,cubemap=cube,generate=generate,quality=quality,filter=filt,tiled=tiled,
                        encoder=o.encoder,source_rgba=pixels.hex(),levels=[dict(width=l['width'],height=l['height'],rgba=l['bgra']) for l in levels],
                        float_container=container.hex()))
                    print(cube,generate,quality,filt,tiled,flush=True)
    containers=[]
    for faces in [1,6]:
        o=HdrOracle()
        o.output_address=o.image(width=4,height=4,fmt=95,count=3,faces=faces)
        containers.append(dict(faces=faces,bc6_container=o.write_container().hex()))
    pathlib.Path(sys.argv[2]).write_text(json.dumps(dict(workbench_sha256=fingerprint,decoder=decoder,rows=rows,containers=containers,
        evidence='Installed Radiance decoder and importer execute unchanged. Encoder format and quality are intercepted; float container is serialized by the installed writer. Each importer case once fresh and twice reused.'),indent=2)+'\n')
