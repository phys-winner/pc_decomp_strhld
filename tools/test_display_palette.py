"""Original paletted sprite fade stores publish one final alpha per pixel."""
import struct, json, re
from pathlib import Path
from test_game_text import Editor
from unicorn import UC_HOOK_MEM_WRITE
from unicorn.x86_const import UC_X86_REG_EIP,UC_X86_REG_EBP,UC_X86_REG_ESP
import display_hooks

ROOT=Path(__file__).resolve().parents[1]
D={
 'classic':dict(renderer=0x21288D8,screen=0xFD5780,flip=0xF105A0,resource=0xF105C8,
                table=0xD7F320,ready=0xD83418,half=0x212892C,frames=0xD0DA58,
                library=0xD7EED8,dims=0xB9A7D8,dispatcher=0x455590,init=0x44E710,raster=0x452000,module='classic'),
 'extreme':dict(renderer=0x2BBBDB8,screen=0xFD5BE0,flip=0xF10A00,resource=0xF10A28,
                table=0xD7F4A0,ready=0xD83598,half=0x2BBBE0C,frames=0xD0DBD8,
                library=0xD7F058,dims=0xB9A958,dispatcher=0x4557D0,init=0x44E950,raster=0x452240,module='game'),
}
STORES={0x277,0x32F,0x493,0x51E,0x74E,0x81F,0xA01,0xAD2}
INTERMEDIATE={0x2E2,0x7CF,0xA82}

def extract(address,folder):
    needle=f'void sub_{address:08X}(void) {{'
    for p in (ROOT/f'generated/{folder}').glob('recomp_*.c'):
        data=p.read_text()
        pos=data.find(needle)
        if pos>=0:
            end=data.find('\nvoid sub_',pos+1)
            return data[pos:end if end>=0 else None]
    raise AssertionError(hex(address))

def proof(edition):
    d=D[edition];e=Editor(edition);u=e.u;out=0x12000000;src=0x10010000;pal=0x10011000
    u.mem_map(out,0x20000)
    r=d['renderer'];s=d['screen'];lib=46
    e.w(r+4,1);e.w(r+0x58,0);e.w(r+0x5C,16);e.w(r+0x60,0);e.w(r+0x64,16)
    e.w(r+0x70,pal);e.w(r+0x78,src);e.w(r+lib*0x1458+0x530,2)
    e.w(d['resource'],lib);e.w(d['flip'],0);e.w(d['library']+lib*4,0);e.w(d['frames'],0)
    e.h(d['dims'],12);e.h(d['dims']+2,4)
    e.w(s+0xD4,out);e.w(s+0xD8,out);e.w(s+0x40,8112)
    body=extract(d['raster'],'classic-x86' if edition=='classic' else 'game-x86')
    patched,_=display_hooks.apply(body.encode(),d['module'])
    lines={int(m.group(1),16):m.group(0) for m in re.finditer(rb'[^\n]*0x([0-9A-F]{8}):[^\n]*',patched)}
    for off in STORES:
        line=lines[d['raster']+off]
        assert b'stronghold_display_blended' in line,(edition,hex(off),line)
        assert b'MEM32(ebp-0x14u)' in line,(edition,hex(off),line)
    for off in INTERMEDIATE:
        assert b'stronghold_display_' not in lines[d['raster']+off],(edition,hex(off))
    events=[]
    def write(uc,access,address,size,value,user):
        if out<=address<out+0x20000:
            pc=u.reg_read(UC_X86_REG_EIP);bp=u.reg_read(UC_X86_REG_EBP)
            events.append((pc,address,value&65535,e.r(bp-0x14),e.r(bp+0x1C)))
    token=u.hook_add(UC_HOOK_MEM_WRITE,write,begin=out,end=out+0x1ffff)
    streams={
      'literal': bytes([11]+[1,2,3]*4+[0x80])*4,
      'repeat': bytes([0x4B,2,0x80])*4,
      'mixed': bytes([0x21,0x42,2,6]+[1,2,3,1,2,3,1]+[0x80])*4,
    }
    cases=0;visited=set();example=None;intermediates=0
    for fmt in (0x555,0x565):
        e.w(s+0x54,fmt);e.run(d['init']);assert u.reg_read(UC_X86_REG_EIP)==e.end
        e.h(d['ready'],1)
        colors=(0,0x7FFF,0x3DEF,0x03E0) if fmt==0x555 else (0,0xFFFF,0x7BEF,0x07E0)
        for i,v in enumerate(colors):e.h(pal+i*2,v)
        key=0x7C1F if fmt==0x555 else 0xF81F
        for half in (0,1):
            e.w(d['half'],half)
            for mode in (0,1):
                e.w(r+4,mode)
                for name,stream in streams.items():
                    u.mem_write(src,stream)
                    for y in (0,1,-1):
                        for amount in range(33):
                            u.mem_write(out,struct.pack('<H',key)*(0x20000//2));events.clear()
                            e.run(d['dispatcher'],(lib,1,0,y,amount),this=r)
                            assert u.reg_read(UC_X86_REG_EIP)==e.end,(edition,fmt,half,mode,name,y,amount,hex(u.reg_read(UC_X86_REG_EIP)))
                            assert u.reg_read(UC_X86_REG_ESP)==e.stack+24
                            if amount==32:
                                assert not events
                            elif amount>0:
                                final=[]
                                for pc,a,v,lut,pitch in events:
                                    off=pc-d['raster'];visited.add(off)
                                    assert off in STORES|INTERMEDIATE,(edition,hex(pc))
                                    assert lut==d['table']+amount*512,(edition,amount,hex(lut))
                                    assert (lut-d['table'])//512*255//32==amount*255//32
                                    assert pitch!=amount or amount==0 # original argument is already replaced by pitch
                                    if off in INTERMEDIATE:intermediates+=1
                                    else:final.append((a,v))
                                assert final,(edition,fmt,half,mode,name,y,amount)
                                assert len({a for a,v in final})==len(final),(edition,'captured pixel twice')
                                if edition=='classic' and fmt==0x565 and half==0 and mode==1 and name=='repeat' and y==0 and amount==31:
                                    example=dict(amount=amount,key=hex(key),first_isolated_pixel=hex(final[0][1]),
                                                 old_coverage=255,new_coverage=255-amount*255//32,
                                                 overwritten_amount_slot=events[0][4],lut=hex(events[0][3]),
                                                 first_final_store=hex(events[0][0]))
                            cases+=1
    u.hook_del(token)
    assert visited==STORES|INTERMEDIATE,(edition,visited,STORES|INTERMEDIATE-visited)
    return dict(sha256=e.sha,cases=cases,stores={hex(d['raster']+off):('skip intermediate' if off in INTERMEDIATE else 'alpha from retained LUT') for off in sorted(visited)},intermediate_writes=intermediates,actual_dispatcher=hex(d['dispatcher']),example=example,
                scope='Unmodified original x86 initializer, format-2 dispatcher and raster; 0..32 fade, 555/565, full/half, HUD/world, literal/repeat/skip, odd/negative Y. Hook source checked against actual original store PCs.')

if __name__=='__main__':
    result={}
    for edition in D:result[edition]=proof(edition)
    (ROOT/'generated/display-palette-original.json').write_text(json.dumps(result,indent=2)+'\n')

    print('Original x86 palette fade: %d fixtures across Classic/Extreme PASS' % sum(r['cases'] for r in result.values()))
