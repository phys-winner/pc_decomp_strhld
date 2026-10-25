"""Split original world/UI passes; apply after touch_hooks for fresh picking."""
import re

seen = set()
LAYOUT = {
    'game': dict(map=0x2DD78A0, frame=0x57D23F, poll=0x468E80,
                 world_call=0x57D2F0, world=0x4E9A40, viewport=0x4E7440,
                 after=0x57D2F5, groups=(0x57D4C4,0x57D4CE,0x57D4D8),
                 group_fn=0x4B1450, overlay=0x4B00D1, present_call=0x57D502, present=0x4707F0,
                 clear_fn=0x471640,clear_clip=0x471663,clip_fn=0x469030,clear_calls=(0x430F77,0x430FAA,0x431FB9,0x4327D3,0x433490,
                                               0x439D2A,0x44997E,0x4499CB,0x4499F2)),
    'classic': dict(map=0x23443C0, frame=0x57CDEF, poll=0x468C80,
                    world_call=0x57CEA0, world=0x4E9690, viewport=0x4E7090,
                    after=0x57CEA5, groups=(0x57D074,0x57D07E,0x57D088),
                    group_fn=0x4B12C0, overlay=0x4AFF41, present_call=0x57D0B2, present=0x4705E0,
                    clear_fn=0x471430,clear_clip=0x471453,clip_fn=0x468E30,clear_calls=(0x430EF7,0x430F2A,0x431DA9,0x4325C3,0x433280,
                                                  0x439B1A,0x44973E,0x44978B,0x4497B2)),
}
RLE_BACKEDGES = {'game':(0x44E90E,0x44E936),'classic':(0x44E6CE,0x44E6F6)}
PREVIEW_STORES = {'game':0x4B5C20,'classic':0x4B5A90}
PROGRESS = {'classic':(0x493DB8,0x1163A84),'game':(0x493F48,0x1163EE4)}
SAVE_PRESENTS = {'classic':(0x4955B9,0x4433E2),'game':(0x495749,0x443623)}
OPAQUE_RUNS = {'classic':((0x454DEF,0x454DFB),(0x454E0F,0x454E1B),
                           (0x454E6B,0x454E7A),(0x454E8D,0x454E9C)),
               'game':((0x45502F,0x45503B),(0x45504F,0x45505B),
                       (0x4550AB,0x4550BA),(0x4550CD,0x4550DC))}
TERRAIN_RASTER = {'classic':(0x452B00,0x454820),'game':(0x452D40,0x454A60)}
BARS = {'classic':(0x4B2650,0x4B2A20),'game':(0x4B27E0,0x4B2BB0)}

def batch_opaque_runs(text,module):
    """Publish each clipped TGX fill/literal run after its final store."""
    total=0
    def function(m):
        nonlocal total
        body=m[0];changed=0
        for store,edge in OPAQUE_RUNS.get(module,()):
            pattern=(rb'if\(stronghold_display_capture_active\) stronghold_display_written\(edi,2\);'
                     rb'( /\* 0x%08X:[^\r\n]+)' % store)
            body,n=re.subn(pattern,rb'_display_run_bytes+=2;\1',body)
            if not n:continue
            boundary=rb'(    if [^\r\n]+/\* 0x%08X:[^\r\n]+\*/)' % edge
            body,k=re.subn(boundary,rb'\1\n    if(stronghold_display_capture_active) stronghold_display_written(edi-_display_run_bytes,_display_run_bytes); _display_run_bytes=0;',body)
            if n!=1 or k!=1:raise ValueError(f'{module}: opaque run {store:08X} changed')
            seen.add(f'{module}:opaque{store:08X}');changed+=1
        if changed:
            body=body.replace(b'{',b'{\n    unsigned _display_run_bytes=0;',1)
            total+=changed
        return body
    return re.sub(rb'(?ms)^void sub_[A-Za-z0-9_]+\(void\) \{.*?^\}',function,text),total

def batch_rle_backedges(text,module):
    """Defer only the original half-size <=32-pixel RLE run checks.

    Count every original edge locally; publish the exact same raw budget at
    the run boundary. The existing outer edges and yield macro stay intact.
    A due yield is delayed by at most the remaining31 pixels of that run.
    """
    total=0
    def function(m):
        nonlocal total
        body=m[0];changed=0
        for pc in RLE_BACKEDGES.get(module,()):
            pattern=(rb'    RECOMP_BACKEDGE\(\);\r?\n'
                     rb'(    if \([^\r\n]+goto L_[0-9A-F]+; /\* 0x%08X:[^\r\n]+\*/)' % pc)
            def replace(edge):
                return (b'    ++_display_rle_budget;\n'+edge[1]+
                        b'\n    { uint32_t _display_edges=_display_rle_budget; _display_rle_budget=0; '
                        b'if(!recomp_yield_hook || (uint64_t)g_backedges+_display_edges<RECOMP_YIELD_EVERY) '
                        b'g_backedges+=_display_edges; else while(_display_edges--){ RECOMP_BACKEDGE(); } }')
            body,n=re.subn(pattern,replace,body)
            if n:seen.add(f'{module}:rle{pc:08X}');changed+=n
        if changed:
            body=body.replace(b'{',b'{\n    uint32_t _display_rle_budget=0;',1)
            total+=changed
        return body
    text=re.sub(rb'(?ms)^void sub_[A-Za-z0-9_]+\(void\) \{.*?^\}',function,text)
    return text,total

def required(module):
    p=LAYOUT.get(module)
    return ({f'{module}:{v:08X}' for v in (p['frame'],p['world_call'],p['after'],
                                         *p['groups'],p['overlay'],p['present_call'],p['clear_clip'],*p['clear_calls'])}|
            {f'{module}:rle{v:08X}' for v in RLE_BACKEDGES[module]}|
            {f'{module}:preview{PREVIEW_STORES[module]:08X}',f'{module}:resource-animation',
             f'{module}:{PROGRESS[module][0]:08X}'}|
            {f'{module}:{pc:08X}' for pc in SAVE_PRESENTS[module]}|
            {f'{module}:opaque{pc:08X}' for pc,_ in OPAQUE_RUNS[module]}|
            {f'{module}:terrain-raster'}|{f'{module}:bar{pc:08X}' for pc in BARS[module]}) if p else set()

def apply(text,module):
    p=LAYOUT.get(module)
    if p is None:return text,0
    text,count=batch_rle_backedges(text,module)
    for pc in BARS[module]:
        entry=b'RECOMP_ENTER(0x%08Xu);' % pc
        n=text.count(entry)
        if n:
            text=text.replace(entry,entry+b' stronghold_display_bar(esp);')
            seen.add(f'{module}:bar{pc:08X}');count+=n
    # These original software raster families include TGX/GM1 UI sprites,
    # rectangle/line primitives and bitmap text. Track performed stores, not
    # RGB values: opaque black, cyan and the desktop key remain drawable.
    # The guard is false throughout the world pass.
    def raster_store(m):
        nonlocal count
        line=m[0]
        pc=int(re.search(rb'/\* 0x([0-9A-F]{8}):',line)[1],16)
        # Exact edition ranges, not an assumed uniform code relocation.
        ranges=((0x44C000,0x455FFF),(0x468000,0x469FFF),(0x471000,0x472FFF)) if module=='game' else (
                (0x44BE00,0x455DFF),(0x467E00,0x469DFF),(0x470DF0,0x472DDF))
        # The SAV thumbnail writer has its own source!=0 gate and lies
        # outside the common raster families. Track only its performed store.
        preview=pc==PREVIEW_STORES[module]
        if not preview and not any(lo<=pc<=hi for lo,hi in ranges):return line
        # These diamond blitters are called only by the world pass, before UI capture.
        if TERRAIN_RASTER[module][0]<=pc<TERRAIN_RASTER[module][1]:
            seen.add(f'{module}:terrain-raster');return line
        original_pc=pc+(0x240 if module=='classic' and 0x44E830<=pc<0x44F8E0 else 0)
        # GM1 blending has an intermediate source write followed by the final
        # source+destination sum. Publish coverage once, after the final sum.
        if original_pc in (0x44ECF0,0x44F16C,0x44F3E9,0x44F87F):return line
        background=None
        if 0x44EA70<=original_pc<0x44F460:
            # The original reuses its amount argument (ebp+1C) for pitch.
            # Its forward lookup pointer is retained throughout both paths.
            base=0xD7F4A0 if module=='game' else 0xD7F320
            background=b'((MEM32(ebp-0xcu)-0x%Xu)/512u*255u/32u)' % base
        elif original_pc in (0x44F513,0x44F58B):
            base=0xD7F4A0 if module=='game' else 0xD7F320
            background=b'((MEM32(ebp+0x18u)-0x%Xu)/512u*255u/32u)' % base
        elif original_pc in (0x44F7EC,0x44F8D5,0x44FA45,0x44FAFE):
            background=b'((32u-((MEM16(MEM32(ebp-4u)-2u)&31u)-MEM32(ebp+0x1cu)))*255u/32u)'
        def store(s):
            nonlocal count
            count+=1
            if preview:seen.add(f'{module}:preview{pc:08X}')
            if background is not None:
                return (s[0]+b' if(stronghold_display_capture_active) stronghold_display_blended('+s[2]+b','+str(int(s[1])//8).encode()+b','+background+b');')
            return (s[0]+b' if(stronghold_display_capture_active) stronghold_display_written('+s[2]+b','+str(int(s[1])//8).encode()+b');')
        return re.sub(rb'MEM(16|32)\(([^;]+?)\) = [^;]+;',store,line)
    text=re.sub(rb'(?m)^    [^\r\n]*/\* 0x[0-9A-F]{8}:[^\r\n]*\*/\r?$',raster_store,text)
    def instruction(address,callee,prefix=b'',suffix=b''):
        nonlocal text,count
        if b'/* 0x%08X:' % address not in text:return
        pattern=rb'^(    )(RECOMP_CALL\(sub_%08X\);)( /\* 0x%08X:[^\r\n]*\*/)(\r?)$' % (callee,address)
        def replace(m):
            return m[1]+prefix+m[2]+suffix+m[3]+m[4]
        text,n=re.subn(pattern,replace,text,flags=re.M)
        if n:seen.add(f'{module}:{address:08X}');count+=n
    cart = 0x434158 if module=='classic' else 0x434368
    instruction(cart,0x455500 if module=='classic' else 0x455740,
                b'stronghold_display_resource_animation(1); ',b' stronghold_display_resource_animation(0);')
    if f'{module}:{cart:08X}' in seen:seen.add(f'{module}:resource-animation')
    instruction(p['frame'],p['poll'],b'stronghold_display_frame_begin(); ')
    address=p['overlay']
    if b'/* 0x%08X:' % address in text:
        pattern=rb'RECOMP_ICALL\(ecx\);( /\* 0x%08X: call ecx \*/)' % address
        text,n=re.subn(pattern,rb'stronghold_display_resource_overlay(esi,1); RECOMP_ICALL(ecx); stronghold_display_resource_overlay(esi,0);\1',text)
        if not n:raise ValueError(f'{module}:{address:08X}: overlay dispatch changed')
        seen.add(f'{module}:{address:08X}');count+=n
    setup=(b'{ stronghold_display_world_begin(0x%08Xu); '
           b'PUSH32(esp, stronghold_display_world_height()); '
           b'PUSH32(esp, stronghold_display_world_width()); PUSH32(esp, 0); PUSH32(esp, 0); '
           b'ecx=0x%08Xu; RECOMP_CALL(sub_%08X); stronghold_display_world_render(0x%08Xu); ecx=0x%08Xu; ' %
           (p['map'],p['map'],p['viewport'],p['map'],p['map']))
    instruction(p['world_call'],p['world'],setup,b' }')
    for address in p['groups']:
        instruction(address,p['group_fn'],
                    b'{ uint32_t _display_root=ecx; stronghold_display_ui_group(_display_root,1); ',
                    b' stronghold_display_ui_group(_display_root,0); }')
    instruction(p['present_call'],p['present'],b'stronghold_display_ui_end(); ')
    progress,position=PROGRESS[module]
    instruction(progress,p['present'],
                b'stronghold_display_progress((int32_t)MEM32(0x%Xu),(int32_t)MEM32(0x%Xu),1); ' % (position,position+4))
    for save in SAVE_PRESENTS[module]:
        instruction(save,p['present'],b'stronghold_display_progress(0,0,0); ')
    # These exact original calls explicitly fill with the transparent HUD
    # clear value. A normal fill, even of the same RGB, remains opaque.
    for address in p['clear_calls']:
        instruction(address,p['clear_fn'],b'stronghold_display_clear_begin(ecx); ',
                    b' stronghold_display_clear_scope(0);')
    # Snapshot the original clipper's inclusive rectangle before its row loop
    # mutates top/height. Only the nine scoped clears enable this branch.
    instruction(p['clear_clip'],p['clip_fn'],suffix=(
                b' if(stronghold_display_clear_batch_active) stronghold_display_clear_clipped(eax);'))
    # touch_hooks owns both calls at the exact post-world instruction.
    address=p['after']
    if b'/* 0x%08X:' % address in text:
        pattern=(rb'stronghold_touch_after_world\(\);\s*'
                 rb'stronghold_display_ui_begin\(0x0*%Xu\);' % p['map'])
        if not re.search(pattern,text):
            raise ValueError(f'{module}: touch hook must resolve before UI capture')
        seen.add(f'{module}:{address:08X}')
    text,batched=batch_opaque_runs(text,module)
    count+=batched
    if count:
        declarations=(b'\nextern void stronghold_display_frame_begin(void);'
                      b'\nextern void stronghold_display_world_begin(uint32_t);'
                      b'\nextern void stronghold_display_world_render(uint32_t);'
                      b'\nextern uint32_t stronghold_display_world_width(void);'
                      b'\nextern uint32_t stronghold_display_world_height(void);'
                      b'\nextern void stronghold_display_ui_begin(uint32_t);'
                      b'\nextern void stronghold_display_ui_group(uint32_t,int);'
                      b'\nextern void stronghold_display_resource_overlay(uint32_t,int);'
                      b'\nextern void stronghold_display_resource_animation(int);'
                      b'\nextern void stronghold_display_ui_end(void);')
        declarations+=b'\nextern void stronghold_display_progress(int,int,int);\nextern void stronghold_display_bar(uint32_t);'
        declarations+=(b'\nextern int stronghold_display_capture_active;'
                       b'\nextern void stronghold_display_written(uint32_t,unsigned);'
                       b'\nextern void stronghold_display_blended(uint32_t,unsigned,unsigned);'
                       b'\nextern void stronghold_display_clear_scope(int);'
                       b'\nextern void stronghold_display_clear_begin(uint32_t);'
                       b'\nextern void stronghold_display_clear_clipped(uint32_t);'
                       b'\nextern int stronghold_display_clear_batch_active;')
        text=text.replace(b'#include "recomp_funcs.h"',b'#include "recomp_funcs.h"'+declarations,1)
    return text,count

if __name__=='__main__':
    from pathlib import Path
    for module in LAYOUT:
        seen.clear();matches=0
        for path in (Path(__file__).resolve().parents[1]/'generated'/f'{module}-x86').glob('recomp_*.c'):
            data=path.read_bytes()
            after=LAYOUT[module]['after']
            # A selfcheck stand-in for the independently verified touch seam.
            marker=rb'(?m)^(    )(RECOMP_CALL\(sub_[0-9A-F]+\); /\* 0x%08X:)' % after
            replacement=(rb'\1stronghold_touch_after_world(); stronghold_display_ui_begin(0x%08Xu);\n\1\2' % LAYOUT[module]['map'])
            data=re.sub(marker,replacement,data)
            patched,n=apply(data,module);matches+=n
            if b'/* 0x%08X:' % after in data and n:
                assert b'stronghold_touch_after_world(); stronghold_display_ui_begin(' in patched
        assert not(required(module)-seen),sorted(required(module)-seen)
        print(f'{module}: {len(seen)} required seams, {matches} lifted copies, touch-before-UI order PASS')
