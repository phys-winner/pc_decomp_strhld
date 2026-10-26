"""Check pan/zoom and hidden-HUD camera limits against the original x86 pick/clamp."""
import json
import math
from pathlib import Path
import struct
from unicorn import UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_EAX, UC_X86_REG_EDI, UC_X86_REG_ECX, UC_X86_REG_ESP
from test_display import Original, ROOT
from test_game_text import Editor


def check(module):
    original=Original(module)
    extreme=module=='game'
    pick=0x4E9180 if extreme else 0x4E8DD0
    coarse=0x4E94C9 if extreme else 0x4E9119
    clamp=0x4E6950 if extreme else 0x4E65A0
    mode_size=0x2BB8D84 if extreme else 0x21258A4
    rotation=0x2BB8C14 if extreme else 0x2125734
    exclusions=0x2BBBE20 if extreme else 0x2128940
    original.w(rotation,0);original.w(exclusions,0);original.w(mode_size,400)
    original.w(original.mode,3)
    original.w(original.obj+0x18B728,0);original.w(original.obj+0x18B72C,0)
    original.w(original.obj+0x18B730,1280);original.w(original.obj+0x18B734,720)
    hook=original.u.hook_add(UC_HOOK_CODE,lambda uc,a,n,user:uc.emu_stop(),begin=coarse,end=coarse)
    def project(camx,camy,x,y,native):
        for offset,value in ((0x78,camx),(0x7C,camy),(0x90,native)):
            original.w(original.obj+offset,value)
        original.u.mem_write(original.sp,struct.pack('<3I',0,x,y))
        original.u.reg_write(UC_X86_REG_ESP,original.sp)
        original.u.reg_write(UC_X86_REG_ECX,original.obj)
        original.u.emu_start(pick,coarse+1,count=1000)
        # EAX is the linear coarse tile, EDI indexes the original sub-tile LUT.
        return original.u.reg_read(UC_X86_REG_EAX),original.u.reg_read(UC_X86_REG_EDI)
    pan_cases=0
    for native in (0,1):
        multiplier=2 if native else 1
        for delta in (1,3,8,16,31,51):
            assert project(3000+delta*multiplier,2000,300,150,native)==project(3000,2000,300+delta,150,native)
            assert project(3000,2000+delta*multiplier,300,150,native)==project(3000,2000,300,150+delta,native)
            pan_cases+=2
    anchor_cases=0
    levels=(.5,.625,.75,1,1.25,1.5,2,2.5)
    for old in levels:
        old_native=int(old<1);old_base=.5 if old_native else 1
        x,y=600,300
        before=project(3000,2000,round(x*old_base/old),round(y*old_base/old),old_native)
        for new in levels:
            native=int(new<1);base=.5 if native else 1
            camx=round(3000+x*(1/old-1/new)+(160 if old_native else 0)-(160 if native else 0))
            camy=round(2000+y*(1/old-1/new))
            after=project(camx,camy,round(x*base/new),round(y*base/new),native)
            assert before==after,(module,old,new,before,after)
            anchor_cases+=1
    original.u.hook_del(hook)
    def stop_ret(uc,address,size,user):
        if uc.mem_read(address,1)[0] in (0xC2,0xC3):uc.emu_stop()
    hook=original.u.hook_add(UC_HOOK_CODE,stop_ret,begin=clamp,end=clamp+0x200)
    bounds=[]
    for zoom in levels:
        native=int(zoom<1);scale=2 if native else 1;residual=zoom/(.5 if native else 1)
        for ui_scale in (.75,1,1.2):
            visible=math.ceil((720-128*ui_scale)/residual)
            render=math.ceil(720/residual)
            original.w(original.obj+0x78,1000000);original.w(original.obj+0x7C,1000000)
            original.w(original.obj+0x90,native)
            original.w(original.obj+0x88,(math.ceil(math.ceil(1280/residual)/32)+5)*scale)
            original.w(original.obj+0x8C,(visible//8+4)*scale)
            original.u.reg_write(UC_X86_REG_ESP,original.sp)
            original.u.reg_write(UC_X86_REG_ECX,original.obj)
            original.u.emu_start(clamp,clamp+0x200,count=1000)
            maximum=original.r(original.obj+0x7C)
            assert maximum==8*(401-(visible//8+4)*scale)
            full_maximum=8*(401-(render//8+4)*scale)
            assert maximum>full_maximum
            bounds.append(dict(zoom=zoom,ui_scale=ui_scale,render_height=render,
                               visible_height=visible,camera_bottom=maximum,
                               additional_reach_over_fullheight_clamp=maximum-full_maximum))
    original.u.hook_del(hook)
    return dict(module=module,original_sha256=original.sha256,pan_cases=pan_cases,
                zoom_under_finger_cases=anchor_cases,bounds_cases=bounds,status='PASS')


def check_hd():
    original=Editor('stronghold');camera=0x16E9358
    original.w(camera,1)
    for at in (0x1503ADC,0x1503B08,0x1503B14):original.w(at,0)
    def project(cx,cy,x,y,native):
        for offset,value in ((0x74,cx),(0x78,cy),(0x8C,native)):
            original.w(camera+offset,value)
        # The real main ground picker, stopped before the sub-tile LUT/terrain reads.
        original.run(0x4B3680,(x,y),camera,lambda pc:original.u.emu_stop() if pc==0x4B3869 else None)
        return original.u.reg_read(UC_X86_REG_EAX),original.u.reg_read(UC_X86_REG_ECX)
    pan_cases=anchor_cases=0
    for native in (0,1):
        factor=2 if native else 1
        for delta in (1,3,8,16,31,51):
            assert project(3000+delta*factor,2000,300,150,native)==project(3000,2000,300+delta,150,native)
            assert project(3000,2000+delta*factor,300,150,native)==project(3000,2000,300,150+delta,native)
            pan_cases+=2
    levels=(.5,.625,.75,1,1.25,1.5,2,2.5)
    for old in levels:
        old_native=int(old<1);old_base=.5 if old_native else 1
        before=project(3000,2000,round(600*old_base/old),round(300*old_base/old),old_native)
        for new in levels:
            native=int(new<1);base=.5 if native else 1
            cx=round(3000+600*(1/old-1/new)+160*old_native-160*native)
            cy=round(2000+300*(1/old-1/new)+8*native-8*old_native)
            assert before==project(cx,cy,round(600*base/new),round(300*base/new),native),(old,new)
            anchor_cases+=1
    # Original 4B5070 rebuilds all mode-specific geometry, before our anchored camera restore.
    original.w(0x1503D38,14);original.w(0x1503D3C,10)
    original.w(0xB18300,1024);original.w(0xB18304,600);original.w(0xB18324,15)
    original.run(0x4B5070,(1,),camera)
    assert original.r(camera+0x8C)==1 and original.r(camera+0x84)==74 and original.r(camera+0x88)==126
    original.run(0x4B5070,(0,),camera)
    assert original.r(camera+0x8C)==0 and original.r(camera+0x84)==37 and original.r(camera+0x88)==63
    return dict(module='stronghold',original_sha256=original.sha,pan_cases=pan_cases,
                zoom_under_finger_cases=anchor_cases,native_mode_rebuild='PASS',
                native_origin_compensation=dict(x=-160,y=8),status='PASS')


if __name__=='__main__':
    result=dict(status='PASS',method='Hash-pinned original x86 executed by Unicorn; real main picker and camera clamp.',
                editions=[check(module) for module in ('game','classic')]+[check_hd()])
    (ROOT/'generated/camera-projection-proof.json').write_text(json.dumps(result,indent=2)+'\n')
    for r in result['editions']:
        print(f"{r['module']}: original picker {r['pan_cases']} pan + {r['zoom_under_finger_cases']} pinch-anchor cases; "
              f"original clamp {len(r.get('bounds_cases',[]))} visible-HUD limits; "
              f"native mode rebuild {r.get('native_mode_rebuild','covered by display tests')}: PASS")
