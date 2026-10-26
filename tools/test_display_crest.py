"""Original custom player crest stores retain fade opacity without reading RGB."""
import struct, json, re
from pathlib import Path
from test_game_text import Editor
from unicorn import UC_HOOK_MEM_WRITE
from unicorn.x86_const import UC_X86_REG_EIP,UC_X86_REG_ESI,UC_X86_REG_EDI,UC_X86_REG_ESP
import display_hooks
ROOT=Path(__file__).resolve().parents[1]
D={
 'classic':dict(renderer=0x21288D8,screen=0xFD5780,key=0xDF53E8,mask=0xF69C04,width=0xF69C10,raster=0x44D0B0,module='classic',folder='classic-x86'),
 'extreme':dict(renderer=0x2BBBDB8,screen=0xFD5BE0,key=0xDF5568,mask=0xF6A064,width=0xF6A070,raster=0x44D2F0,module='game',folder='game-x86'),
}
def extract(address,folder):
    needle=f'void sub_{address:08X}(void) {{'
    for p in (ROOT/f'generated/{folder}').glob('recomp_*.c'):
        b=p.read_text();a=b.find(needle)
        if a<0:continue
        z=b.find('\nvoid sub_',a+1);return b[a:z if z>=0 else None]
    raise AssertionError(hex(address))

def compose(pixel,below,alpha,key,fmt):
    result=0
    for shift,limit in ((0,31),(5,31 if fmt==0x555 else 63),(10 if fmt==0x555 else 11,31)):
        p,b,k=[v>>shift&limit for v in (pixel,below,key)]
        n=int((p*255+(b-k)*(255-alpha)+127)/255)
        result|=max(0,min(limit,n))<<shift
    return result

def proof(edition):
    d=D[edition];e=Editor(edition);u=e.u;r=d['renderer'];s=d['screen'];out=0x12000000;src=0x10012000
    u.mem_map(out,0x100000)
    e.w(r+0x94,src);e.w(s+0xD4,out);e.w(s+0xD8,out);e.w(s+0x40,192);e.w(d['mask'],out);e.w(d['width'],96)
    body=extract(d['raster'],d['folder']);patched,_=display_hooks.apply(body.encode(),d['module'])
    known={int(m[1],16):m[0] for m in re.finditer(rb'[^\n]*MEM16\(ecx\) =[^\n]*/\* 0x([0-9A-F]{8}):[^\n]*',patched)}
    events=[]
    def write(uc,access,address,size,value,user):
        pc=u.reg_read(UC_X86_REG_EIP)
        if out<=address<out+0x100000:
            events.append((pc,address,value&65535,u.reg_read(UC_X86_REG_ESI),u.reg_read(UC_X86_REG_EDI)))
    token=u.hook_add(UC_HOOK_MEM_WRITE,write,begin=out,end=out+0xfffff)
    cases=0;visited=set();maxerror=0;oldmax=0;example=None
    for fmt in (0x555,0x565):
        e.w(s+0x54,fmt);key=0x7C1F if fmt==0x555 else 0xF81F;e.h(d['key'],key)
        source=[key]*4224
        colors=(0,0x3FF if fmt==0x555 else 0x7FF,0x2104,0x7FFF if fmt==0x555 else 0xFFFF)
        for i,c in enumerate(colors):source[i]=c;source[-4+i]=c
        u.mem_write(src,struct.pack('<4224H',*source))
        for mode in (0,1,2):
            e.w(r+4,mode)
            for amount in range(33):
                def run(background):
                    u.mem_write(out,struct.pack('<H',background)*(0x100000//2));events.clear()
                    e.run(d['raster'],(0,3,1,amount),this=r)
                    assert u.reg_read(UC_X86_REG_EIP)==e.end,(edition,fmt,mode,amount,hex(u.reg_read(UC_X86_REG_EIP)))
                    assert u.reg_read(UC_X86_REG_ESP)==e.stack+20
                    return list(events)
                captured=run(key)
                if amount==32:assert not captured;cases+=1;continue
                assert len(captured)==8,(edition,fmt,mode,amount,len(captured))
                alpha=255-amount*255//32 if fmt==0x565 else 255
                if amount:
                    for pc,addr,pixel,esi,edi in captured:
                        assert esi==amount and (fmt!=0x565 or edi==32-amount),(edition,hex(pc),amount,esi,edi)
                        assert pc in known,(edition,hex(pc))
                        visited.add(pc)
                for below in (0,0x7FFF if fmt==0x555 else 0xFFFF,0x3FF if fmt==0x555 else 0x7FF,0x7C00 if fmt==0x555 else 0xF800):
                    original=run(below)
                    assert [a for pc,a,p,si,di in original]==[a for pc,a,p,si,di in captured]
                    for old,new in zip(captured,original):
                        pixel=compose(old[2],below,alpha,key,fmt)
                        for shift,limit in ((0,31),(5,31 if fmt==0x555 else 63),(10 if fmt==0x555 else 11,31)):
                            error=abs((pixel>>shift&limit)-(new[2]>>shift&limit));maxerror=max(maxerror,error)
                            oldmax=max(oldmax,abs((old[2]>>shift&limit)-(new[2]>>shift&limit)))
                            assert error<=2,(edition,fmt,mode,amount,hex(pixel),hex(new[2]),alpha)
                if edition=='classic' and fmt==0x565 and mode==1 and amount==31:
                    example=dict(amount=amount,source=hex(colors[0]),key=hex(key),isolated_pixel=hex(captured[0][2]),
                                 old_alpha=255,correct_alpha=alpha,actual_store=hex(captured[0][0]),retained_esi=captured[0][3])
                cases+=1
    u.hook_del(token)
    assert visited==set(known),(edition,[hex(v) for v in visited],[hex(v) for v in known])
    mapping={}
    for pc,line in known.items():
        expected=b'stronghold_display_written(ecx,2)' if pc==d['raster']+0x4EA else b'stronghold_display_blended(ecx,2,(esi*255u/32u))'
        assert expected in line.replace(b' ',b''),(edition,hex(pc),line)
        mapping[hex(pc)]='source darkening: opaque555 preserved' if pc==d['raster']+0x4EA else 'fade: retained esi/32'
    return dict(sha256=e.sha,cases=cases,actual_original_function=hex(d['raster']),visited_stores=[hex(v) for v in sorted(visited)],
                original_esi_proven='amount retained at every performed store',max_channel_error=maxerror,
                old_opaque_max_channel_error=oldmax,hook_mapping=mapping,example=example)
if __name__=='__main__':
    result={}
    for edition in D:result[edition]=proof(edition)
    (ROOT/'generated/display-crest-original.json').write_text(json.dumps(result,indent=2)+'\n')

    print('Original x86 crest fade: %d fixtures across Classic/Extreme PASS' % sum(r['cases'] for r in result.values()))
