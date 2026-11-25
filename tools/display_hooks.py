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
RLE_BACKEDGES = {'game':(0x44E90E,0x44E936),'classic':(0x44E6CE,0x44E6F6),'stronghold':(0x4419CE,0x4419F6)}
HALF_RUNS = {'classic':((0x44E6BC,0x44E6D0,False),(0x44E6DE,0x44E6F8,True)),
             'game':((0x44E8FC,0x44E910,False),(0x44E91E,0x44E938,True)),
             'stronghold':((0x4419BC,0x4419D0,False),(0x4419DE,0x4419F8,True))}
HALF_PALETTE = {
    'classic': (0x451F0F, (0x44FDF0, 0x451E33)),
    'game': (0x45214F, (0x450030, 0x450043, 0x450044, 0x45004C, 0x45004E,
                        0x450052, 0x450056, 0x450073, 0x452073)),
    'stronghold': (0x4451E7, (0x4430F0, 0x44510B, 0x44524F)),
}
PREVIEW_STORES = {'game':0x4B5C20,'classic':0x4B5A90}
PROGRESS = {'classic':(0x493DB8,0x1163A84),'game':(0x493F48,0x1163EE4)}
SAVE_PRESENTS = {'classic':(0x4955B9,0x4433E2),'game':(0x495749,0x443623)}
OPAQUE_RUNS = {'classic':((0x454DEF,0x454DFB),(0x454E0F,0x454E1B),
                           (0x454E6B,0x454E7A),(0x454E8D,0x454E9C)),
               'game':((0x45502F,0x45503B),(0x45504F,0x45505B),
                       (0x4550AB,0x4550BA),(0x4550CD,0x4550DC)),
               'stronghold':((0x44824F,0x44825B),(0x44826F,0x44827B),
                             (0x4482CB,0x4482DA),(0x4482ED,0x4482FC))}
HUD_LITERAL64 = {'classic':0x44DEE0,'game':0x44E120}
HUD_KEYED = {'classic':(0x455205,1),'game':(0x455445,2)}
HUD_LITERAL_SPANS = (
    0x44D812,0x44D82D,0x44D850,0x44D871,0x44D89A,0x44D8C1,0x44D8F0,0x44D91D,
    0x44D952,0x44D985,0x44D9C0,0x44D9F9,0x44DA3A,0x44DA79,0x44DAC0,0x44DB05,
    0x44DB52,0x44DB9D,0x44DBF0,0x44DC41,0x44DC9A,0x44DCF1,0x44DD50,0x44DDAD,
    0x44DE12,0x44DE75,
)
HUD_SOLID_SPANS = (
    0x44DF5E,0x44DF6C,0x44DF79,0x44DF8A,0x44DF9A,0x44DFAE,0x44DFC1,0x44DFD8,
    0x44DFEE,0x44E008,0x44E021,0x44E03E,0x44E05A,0x44E07A,0x44E099,0x44E0BC,
    0x44E0DE,0x44E104,0x44E129,0x44E152,0x44E17A,0x44E1A6,0x44E1D1,0x44E200,
    0x44E22E,0x44E260,0x44E291,0x44E2C6,0x44E2FA,0x44E332,
)
HUD_SPAN_SHIFT = {'classic':0,'game':0x240}
HUD_SPANS = {
    module: {
        pc+shift:(size,solid)
        for pcs,sizes,solid in ((HUD_LITERAL_SPANS,range(12,64,2),False),
                                (HUD_SOLID_SPANS,range(6,66,2),True))
        for pc,size in zip(pcs,sizes)
    }
    for module,shift in HUD_SPAN_SHIFT.items()
}
HUD_SPAN_LABELS = {
    module: re.compile(rb'(?m)^L_('+b'|'.join(b'%08X'%pc for pc in spans)+rb'):\r?\n')
    for module,spans in HUD_SPANS.items()
}
TERRAIN_RASTER = {'classic':(0x452B00,0x454820),'game':(0x452D40,0x454A60)}
TERRAIN_COPIES = {'classic':0x452B00,'game':0x452D40,'stronghold':0x445DB0}
FILL_ROWS = {'classic':0x468F90,'game':0x469190,'stronghold':0x459D00}
BARS = {'classic':(0x4B2650,0x4B2A20),'game':(0x4B27E0,0x4B2BB0)}
PALETTE_FADE = {'classic':(0x452000,0xD7F320),'game':(0x452240,0xD7F4A0)}
PALETTE_FADE_STORES = (0x277,0x32F,0x493,0x51E,0x74E,0x81F,0xA01,0xAD2)
PALETTE_FADE_INTERMEDIATE = (0x2E2,0x7CF,0xA82)
# RGB555 only darkens the source; these RGB565 stores blend with the destination.
CREST_FADE_STORES = {'classic':(0x44D260,0x44D337,0x44D40E,0x44D4E5),
                     'game':(0x44D4A0,0x44D577,0x44D64E,0x44D725)}
RLE_STACK_FRAME = {'classic':0x44D5D0,'game':0x44D810,'stronghold':0x4408D0}
HALF_TGX_ENTRY = {'classic':0x44E577,'game':0x44E7B7,'stronghold':0x441877}
RLE_STACK_STORES = frozenset((0x9,0xC,0x13,0x38,0x3E,0x41,0x47,0x65,0x68,0x75,0x7E,
    0x9E,0xA9,0xAC,0xB6,0xD2,0xDA,0xE3,0xEC,0x108,0x156,0xFB3,0xFBA,0xFC7,0xFE9,
    0x1028,0x106F,0x10CB,0x10D8,0x10E9,0x110B))


def raster_stack_store(line,pc,module):
    # These exact TGX instructions use EBP only as the original function's stack frame.
    entry=RLE_STACK_FRAME.get(module)
    return entry is not None and pc-entry in RLE_STACK_STORES and re.match(
        rb'\s*MEM32\(ebp \+ (?:\(-0x[0-9A-Fa-f]+\)|0x14)\) =',line) is not None

def collapse_terrain_copies(text,module):
    """Keep the three original diamond layouts; combine their independent pixel copies."""
    base=TERRAIN_COPIES.get(module)
    if base is None:return text,0
    pattern=rb'(?ms)^void sub_%08X\(void\) \{.*?^\}'%base
    def function(match):
        body=match[0]
        for first,last,width in ((0x7B,0x617,4),(0x653,0x9A2,2),(0x9A7,0xCF4,2)):
            span=(rb'(?ms)^    [^\r\n]+/\* 0x%08X:[^\r\n]+\*/\r?\n.*?(?=^(?:    [^\r\n]+/\* 0x%08X:|L_%08X:))'
                  %(base+first,base+last,base+last))
            original=re.search(span,body)
            if not original:raise ValueError(f'{module}: terrain copy block {base+first:08X} changed')
            registers={};loads={};pixels=[]
            for line in original[0].splitlines():
                instruction=line.split(b'/*')[0].strip()
                load=re.fullmatch(rb'(e[a-d]x) = MEM32\(esi(?: \+ (0x[0-9A-F]+))?\);',instruction) if width==4 else re.fullmatch(
                    rb'SET_LO16\((e[a-d]x), MEM16\(esi(?: \+ (0x[0-9A-F]+))?\)\);',instruction)
                if load:
                    registers[load[1]]=int(load[2] or b'0',16);loads[load[1]]=line;continue
                store=re.fullmatch(rb'MEM32\(edi \+ (0x[0-9A-F]+)\) = (e[a-d]x);',instruction) if width==4 else re.fullmatch(
                    rb'MEM16\(edi \+ (0x[0-9A-F]+)\) = \(uint16_t\)\(LO16\((e[a-d]x)\)\);',instruction)
                if not store or store[2] not in registers:raise ValueError(f'{module}: unexpected terrain copy instruction {instruction!r}')
                pixels.append((int(store[1],16),registers[store[2]]))
            if len(pixels)!=(128 if width==4 else 64):raise ValueError(f'{module}: terrain copy size changed')
            runs=[];stride=4
            for dst,src in pixels:
                if runs and dst==runs[-1][0]+runs[-1][2]*width and src==runs[-1][1]+runs[-1][2]*stride:
                    runs[-1][2]+=1
                else:runs.append([dst,src,1])
            source_end=max(src+width for _,src in pixels);target_end=max(dst+width for dst,_ in pixels)
            code=[b'    if((uint64_t)esi+%du<=UINT32_MAX && (uint64_t)edi+%du<=UINT32_MAX &&'% (source_end,target_end),
                  b'       ((uint64_t)esi+%du<=edi || (uint64_t)edi+%du<=esi)) {'%(source_end,target_end),
                  b'        const uint8_t *__restrict _terrain_s=(const uint8_t*)GPTR(esi);',
                  b'        uint8_t *__restrict _terrain_d=(uint8_t*)GPTR(edi);']
            if width==2:
                code.extend((b'        typedef uint16_t _terrain_v8 __attribute__((vector_size(16)));',
                             b'        typedef uint16_t _terrain_v4 __attribute__((vector_size(8)));'))
            for dst,src,n in runs:
                if width==4:code.append(b'        __builtin_memcpy(_terrain_d+%du,_terrain_s+%du,%du);'%(dst,src,n*4))
                else:
                    copied=0
                    while n-copied>=4:
                        lanes=8 if n-copied>=8 else 4
                        if src+copied*4+lanes*4>source_end:break
                        indices=b','.join(str(i*2).encode() for i in range(lanes))
                        code.append(b'        { _terrain_v%d _a,_b,_c; '%lanes+
                            b'__builtin_memcpy(&_a,_terrain_s+%du,%du); '%(src+copied*4,lanes*2)+
                            b'__builtin_memcpy(&_b,_terrain_s+%du,%du); '%(src+copied*4+lanes*2,lanes*2)+
                            b'_c=__builtin_shufflevector(_a,_b,'+indices+b'); '+
                            b'__builtin_memcpy(_terrain_d+%du,&_c,%du); }'%(dst+copied*2,lanes*2))
                        copied+=lanes
                    for pixel in range(copied,n):
                        code.append(b'        __builtin_memcpy(_terrain_d+%du,_terrain_s+%du,2);'%(dst+pixel*2,src+pixel*4))
            code.extend(loads.values())
            code.extend((b'    } else {',original[0].rstrip(b'\r\n'),b'    }\n'))
            body=body[:original.start()]+b'\n'.join(code)+body[original.end():]
        seen.add(f'{module}:terrain-copy')
        return body
    text,n=re.subn(pattern,function,text)
    return text,n*3


def batch_hud_literal64(text,module):
    pc=HUD_LITERAL64[module]
    entries=list(re.finditer(rb'(?m)^L_%08X:\r?\n'%pc,text))
    if not entries:return text,0
    if len(entries)!=1:raise ValueError(f'{module}: duplicate HUD literal entry {pc:08X}')
    start=entries[0].end()
    stop=text.find(b'    _flag_a = (uint32_t)(esi); _flag_b = (uint32_t)(0x40u);',start,start+4096)
    original=text[start:stop]
    statements=[]
    for base in range(0,64,12):
        copies=[(reg,b' + 0x%X'%offset if offset else b'')
                for reg,offset in zip((b'eax',b'ebx',b'ecx'),range(base,min(base+12,64),4))]
        statements.extend(b'    %s = MEM32(esi%s);'%(reg,offset) for reg,offset in copies)
        statements.extend(
            b'    MEM32(edi%s) = %s; if(stronghold_display_capture_active) stronghold_display_written(edi%s,4);'
            %(offset,reg,offset) for reg,offset in copies)
    if stop<0 or re.sub(rb' /\* 0x[0-9A-F]{8}: mov [^\r\n]+\*/',b'',original).splitlines()!=statements:
        raise ValueError(f'{module}: HUD literal moves at {pc:08X} changed')
    fast=(b'    if(stronghold_display_capture_active && stronghold_display_hud_literal64(edi,esi)){\n'
          b'        eax = MEM32(esi + 0x3C);\n'
          b'        ebx = MEM32(esi + 0x34);\n'
          b'        ecx = MEM32(esi + 0x38);\n'
          b'    }else{\n')
    seen.add(f'{module}:hud-literal64')
    return text[:start]+fast+original+b'    }\n'+text[stop:],1


def batch_hud_spans(text,module):
    """Check the exact TGX loads and stores before replacing a visible span."""
    pieces=[]
    found=set()
    previous=0
    for match in HUD_SPAN_LABELS[module].finditer(text):
        pc=int(match[1],16)
        if pc in found:raise ValueError(f'{module}: duplicate HUD span {pc:08X}')
        size,solid=HUD_SPANS[module][pc]
        start=match.end()
        reg=b'edi' if solid else b'esi'
        width=b'%d'%size if size<10 else b'0x%Xu'%size
        boundary=b'    _flag_a = (uint32_t)(%s); _flag_b = (uint32_t)(%s);'%(reg,width)
        stop=text.find(boundary,start,start+4096)
        original=text[start:stop]
        statements=re.sub(rb' /\* 0x[0-9A-F]{8}: mov [^\r\n]+\*/',b'',original).splitlines()
        expected,full,tail=[],{},[]
        for base in range(0,size,12):
            stores=[]
            for register,offset in zip((b'eax',b'ebx',b'ecx'),range(base,min(base+12,size),4)):
                at=b' + 0x%X'%offset if offset else b''
                short=offset+4>size
                if solid:
                    register=b'eax'
                elif short:
                    load=b'    SET_LO16(%s, MEM16(esi%s));'%(register,at)
                    expected.append(load)
                    tail.append(load)
                else:
                    load=b'    %s = MEM32(esi%s);'%(register,at)
                    expected.append(load)
                    full[register]=load
                value=b'(uint16_t)(LO16(%s))'%register if short else register
                bits=16 if short else 32
                stores.append(b'    MEM%d(edi%s) = %s; if(stronghold_display_capture_active) stronghold_display_written(edi%s,%d);'
                              %(bits,at,value,at,bits//8))
            expected.extend(stores)
        end=rb'    RECOMP_BACKEDGE\(\);\r?\n    goto L_%08X;'%(0x44D768+HUD_SPAN_SHIFT[module])
        if stop<0 or statements!=expected or not re.search(end,text[stop:stop+600]):
            raise ValueError(f'{module}: HUD span {pc:08X} changed')
        helper,arg=(b'solid',b'eax') if solid else (b'literal',b'esi')
        fast=(b'    if(stronghold_display_capture_active && stronghold_display_hud_%s_span(edi,%s,%d)){\n'
              %(helper,arg,size))
        fast+=b''.join(b'    '+line+b'\n' for line in [*full.values(),*tail])+b'    }else{\n'
        pieces.extend((text[previous:start],fast,original,b'    }\n'))
        previous=stop
        found.add(pc)
        seen.add(f'{module}:hud-span{pc:08X}')
    if not found:return text,0
    return b''.join((*pieces,text[previous:])),len(found)


def batch_hud_keyed(text,module):
    pc,copies=HUD_KEYED[module]
    label=rb'(?m)^L_%08X:\r?\n'%pc
    entries=len(re.findall(label,text))
    if not entries:return text,0
    end=pc+22
    expected=(
        b'    SET_LO16(eax, MEM16(esi));\n'
        b'    _flag_a = (uint32_t)(LO16(eax)) << 16; _flag_b = (uint32_t)(LO16(ebx)) << 16;\n'
        b'    _cf = (uint32_t)CMP_B(_flag_a, _flag_b);\n'
        b'    _flag_k = FK_NARROW(FK_CMP, 16);\n'
        b'    if (CMP_EQ(_flag_a, _flag_b)) goto L_%08X;\n'
        b'L_%08X:\n'
        b'    MEM16(edi) = (uint16_t)(LO16(eax)); if(stronghold_display_capture_active) stronghold_display_written(edi,2);\n'
        b'L_%08X:\n'
        b'    _flag_a = (uint32_t)(esi); _flag_b = (uint32_t)(2);\n'
        b'    _cf = (uint32_t)((uint32_t)(_flag_a + _flag_b) < (uint32_t)_flag_a);\n'
        b'    esi = esi + 2;\n'
        b'    _flag_k = FK_ADD;\n'
        b'    _flag_a = (uint32_t)(edi); _flag_b = (uint32_t)(2);\n'
        b'    _cf = (uint32_t)((uint32_t)(_flag_a + _flag_b) < (uint32_t)_flag_a);\n'
        b'    edi = edi + 2;\n'
        b'    _flag_k = FK_ADD;\n'
        b'    _flag_a = (uint32_t)(edx); _flag_b = (uint32_t)(1);\n'
        b'    _cf = (uint32_t)CMP_B(_flag_a, _flag_b);\n'
        b'    edx = edx - 1;\n'
        b'    _flag_k = FK_CMP;\n'
        b'    RECOMP_BACKEDGE();\n'
        b'    if ( CMP_NE(_flag_a, _flag_b)) goto L_%08X;'%(pc+11,pc+8,pc+11,pc))
    fast=(b'    if(stronghold_display_capture_active && edx>=8u && edx<=4056u &&\n'
          b'       (!recomp_yield_hook||(uint64_t)g_backedges+edx<RECOMP_YIELD_EVERY) &&\n'
          b'       stronghold_display_hud_keyed(edi,esi,edx,(uint16_t)ebx)){\n'
          b'        SET_LO16(eax,MEM16(esi+2u*edx-2u));\n'
          b'        esi+=2u*edx; edi+=2u*edx; g_backedges+=edx; edx=0;\n'
          b'        _flag_a=_flag_b=1; _cf=0; _flag_k=FK_CMP;\n'
          b'        goto L_%08X;\n    }\n'%end)

    def replace(m):
        contents=re.sub(rb'/\*[^\n]*?\*/',b'',m[2])
        statements=b'\n'.join(line.rstrip() for line in contents.splitlines() if line.strip())
        if statements!=expected:raise ValueError(f'{module}: keyed HUD row {pc:08X} changed')
        return m[1]+fast+m[2]

    text,count=re.subn(rb'(?ms)^(L_%08X:\r?\n)(.*?)(?=^L_%08X:\r?$)'%(pc,end),replace,text)
    if count!=entries or count!=copies:raise ValueError(f'{module}: keyed HUD row copies changed')
    seen.add(f'{module}:hud-keyed')
    return text,count


def batch_opaque_runs(text,module):
    """Write clipped opaque TGX runs without dispatching each pixel."""
    total=0
    def function(m):
        nonlocal total
        body=m[0];changed=0
        for index,(store,edge) in enumerate(OPAQUE_RUNS.get(module,())):
            pattern=(rb'if\s*\(stronghold_display_capture_active\)\s*stronghold_display_written\(edi,\s*2\);'
                     rb'( /\* 0x%08X:[^\r\n]+)' % store)
            body,n=re.subn(pattern,rb'_display_run_bytes+=2;\1',body)
            if not n:continue
            boundary=rb'(    if [^\r\n]+/\* 0x%08X:[^\r\n]+\*/)' % edge
            body,k=re.subn(boundary,rb'\1\n    if(stronghold_display_capture_active) stronghold_display_written(edi-_display_run_bytes,_display_run_bytes); _display_run_bytes=0;',body)
            if n!=1 or k!=1:raise ValueError(f'{module}: opaque run {store:08X} changed')
            literal=index>=2
            start=store-3 if literal else store
            entry=rb'(?m)^(L_%08X:\r?\n)'%start
            exit_pc=edge+2
            if (b'goto L_%08X; /* 0x%08X:'%(start,edge) not in body or
                    b'L_%08X:'%exit_pc not in body):
                raise ValueError(f'{module}: opaque TGX loop {start:08X} changed')
            nonoverlap=(b' && ((uint64_t)esi+ecx*2<=edi || (uint64_t)edi+ecx*2<=esi)' if literal else b'')
            copy=(b'SET_LO16(eax,MEM16(esi+_bytes-2)); memcpy((void*)ADDR(edi),(const void*)ADDR(esi),_bytes); esi+=_bytes;'
                  if literal else
                  b'for(uint32_t _i=0;_i<_bytes;_i+=2) MEM16(edi+_i)=(uint16_t)LO16(eax);')
            fast=(b'    if(!_display_run_bytes && ecx && ecx<=32 && (!recomp_yield_hook || (uint64_t)g_backedges+ecx<RECOMP_YIELD_EVERY)'
                  + nonoverlap + b'){\n    uint32_t _bytes=ecx*2; '+copy+
                  b'\n    if(stronghold_display_capture_active) stronghold_display_written(edi,_bytes);'
                  b'\n    edi+=_bytes; ebx+=ecx; g_backedges+=ecx; ecx=0;'
                  b' _flag_a=_flag_b=1; _cf=0; _flag_k=FK_CMP; goto L_%08X;\n    }\n'%exit_pc)
            body,n=re.subn(entry,lambda m:m[1]+fast,body)
            if n!=1:raise ValueError(f'{module}: opaque TGX entry {start:08X} changed')
            seen.add(f'{module}:opaque{store:08X}');changed+=1
        if changed:
            body=body.replace(b'{',b'{\n    unsigned _display_run_bytes=0;',1)
            total+=changed
        return body
    return re.sub(rb'(?ms)^void sub_[A-Za-z0-9_]+\(void\) \{.*?^\}',function,text),total

def collapse_half_runs(text,module):
    """Copy the selected half-size pixels; retain the original out-of-range path."""
    count=0
    for start,end,literal in HALF_RUNS.get(module,()):
        pattern=rb'(L_%08X:\r?\n)' % start
        if not re.search(pattern,text):continue
        copy=(b'for(uint32_t i=0;i<_display_selected;i++){ '
              b'SET_LO16(eax,MEM16(esi+(_display_first+2*i)*2)); '
              b'MEM16(_display_to+2*i)=(uint16_t)LO16(eax); } esi+=_display_count*2;' if literal else
              b'for(uint32_t i=0;i<_display_selected;i++)MEM16(_display_to+2*i)=(uint16_t)LO16(eax);')
        fast=(b'    if(ecx&&ecx<=32){ uint32_t _display_count=ecx; '
              b'uint32_t _display_first=ebx&1u,_display_selected=(ecx+1-_display_first)/2; '
              b'uint32_t _display_to=edi+ebx+_display_first; '+copy+
              b' if(stronghold_display_capture_active&&_display_selected) '
              b'stronghold_display_written(_display_to,_display_selected*2); '
              b'ebx+=_display_count;ecx=0;_flag_a=_flag_b=1;_cf=0;_flag_k=FK_CMP; '
              b'if(!recomp_yield_hook||(uint64_t)g_backedges+_display_count<RECOMP_YIELD_EVERY) '
              b'g_backedges+=_display_count;else while(_display_count--){RECOMP_BACKEDGE();} '
              b'goto L_%08X; }\n' % end)
        text,n=re.subn(pattern,lambda m:m[1]+fast,text)
        if n:seen.add(f'{module}:half{start:08X}');count+=n
    return text,count


def collapse_fill_row(text,module):
    base=FILL_ROWS.get(module)
    if base is None:return text,0
    entry=b'void sub_%08X(void) {' % base
    if entry not in text:return text,0
    anchor=(rb'(?m)^(    edx = 2; /\* 0x%08X: mov edx, 2 \*/\r?\n)(?=L_%08X:)'
            % (base+0x45,base+0x4A))
    store=b'MEM16(edi) = (uint16_t)(LO16(ecx)); /* 0x%08X: mov word ptr [edi], cx */' % (base+0x4E)
    edge=b'goto L_%08X; /* 0x%08X:' % (base+0x4A,base+0x56)
    if store not in text or edge not in text:raise ValueError(f'{module}: solid row changed')
    fast=b'''    {
        extern int stronghold_display_capture_active;
        if (!stronghold_display_capture_active && eax <= ebx && ebx < 4056u) {
            uint32_t _row_pixels = ebx - eax + 1;
            if ((uint64_t)edi + 2u * _row_pixels <= UINT32_MAX &&
                (!recomp_yield_hook || (uint64_t)g_backedges + _row_pixels < RECOMP_YIELD_EVERY)) {
                uint8_t *_row = (uint8_t *)GPTR(edi);
                uint16_t _color = (uint16_t)ecx;
                for (uint32_t _i = 0; _i < _row_pixels; ++_i)
                    __builtin_memcpy(_row + 2u * _i, &_color, sizeof _color);
                edi += 2u * _row_pixels;
                eax += _row_pixels;
                g_backedges += _row_pixels;
            }
        }
    }
'''
    text,count=re.subn(anchor,lambda m:m[1]+fast,text)
    if count!=1:raise ValueError(f'{module}: solid row entry changed')
    seen.add(f'{module}:fill-row')
    return text,count


def native_half_commands(text,module,*,palette=False):
    if palette:
        if module not in HALF_PALETTE:return text,0
        start, functions = HALF_PALETTE[module]
        first = rb'ecx = 0;'
        column = b'0x20u'
        palette_fill = b'if (_kind == 2) eax = MEM16(edx + 2u * MEM8(esi + 1));'
        raw_fill = b''
        literal = b'MEM16(edx + 2u * MEM8(esi + _first + 2u * _i))'
        payload = b'esi += _length;'
        # Keep the lifted per-pixel HUD callbacks for clipped surface writes.
        guard = b'if (stronghold_display_capture_active) break;'
        coverage = b''
    else:
        if module not in RLE_STACK_FRAME:return text,0
        base = RLE_STACK_FRAME[module]
        start, functions = base + 0x1075, (base, HALF_TGX_ENTRY[module])
        first = rb'SET_LO8\(eax, MEM8\(esi\)\);'
        column = b'0x1Cu'
        palette_fill = b''
        raw_fill = b'eax = MEM16(esi);'
        literal = b'MEM16(esi + (_first + 2u * _i) * 2u)'
        payload = b'esi += 2u * _length;'
        guard = b''
        coverage = b'''if (stronghold_display_capture_active && _pixels)
                stronghold_display_written(_dest, 2u * _pixels);'''
    if b'L_%08X:' % start not in text:return text,0
    entry = rb'(?m)^(L_%08X:\r?\n)(?=    %s /\* 0x%08X:)' % (start,first,start)
    fast=b'''    /* Leave row transitions and a pending scheduler yield to the lifted decoder. */
    for (;;) {
        extern int stronghold_display_capture_active;
        extern void stronghold_display_written(uint32_t,unsigned);
        %s
        unsigned _cmd = MEM8(esi), _kind = _cmd >> 5, _length = (_cmd & 31u) + 1;
        if (_kind > 2) break;
        unsigned _edges = _kind == 1 ? 1 : _length + 1;
        if (recomp_yield_hook && (uint64_t)g_backedges + _edges >= RECOMP_YIELD_EVERY) break;
        uint32_t _column = MEM32(ebp - %s);
        %s
        MEM32(ebp - %s) = _column + _length;
        ++esi;
        if (_kind == 1) {
            SET_LO8(eax, 0x20u);
            ecx = _length;
            _flag_a = _column; _flag_b = _length;
            _cf = _column + _length < _column; _flag_k = FK_ADD;
        } else {
            unsigned _first = _column & 1u, _pixels = (_length + 1 - _first) / 2;
            uint32_t _dest = edi + _column + _first;
            if (_kind == 2) {
                %s
                for (unsigned _i = 0; _i < _pixels; ++_i)
                    MEM16(_dest + 2u * _i) = (uint16_t)eax;
                %s
            } else {
                %s
                for (unsigned _i = 0; _i < _pixels; ++_i) {
                    SET_LO16(eax, %s);
                    MEM16(_dest + 2u * _i) = (uint16_t)eax;
                }
                %s
            }
            %s
            ebx = _column + _length;
            ecx = 0;
            _flag_a = _flag_b = 1; _cf = 0; _flag_k = FK_CMP;
        }
        g_backedges += _edges;
    }
''' % (guard,column,palette_fill,column,raw_fill,b'++esi;' if palette else b'esi += 2;',
       b'eax = 0;' if palette else b'SET_LO8(eax, 0);',literal,payload,coverage)
    count = 0

    def patch(body):
        nonlocal count
        address = int(body[1],16)
        patched,sites = re.subn(entry,lambda m:m[1]+fast,body[0])
        if sites != 1:raise ValueError(f'{module}: half-size command dispatch changed in {address:08X}')
        kind = 'half-palette' if palette else 'half-commands'
        seen.add(f'{module}:{kind}{address:08X}')
        count += sites
        return patched

    names = b'|'.join(b'%08X' % address for address in functions)
    text = re.sub(rb'(?ms)^void sub_(' + names + rb')\(void\) \{.*?^\}',patch,text)
    return text,count


def batch_run_backedges(text,module,edges,label):
    """Defer only the proven <=32-pixel RLE/TGX run checks.

    Count every original edge locally; publish the exact same raw budget at
    the run boundary. The existing outer edges and yield macro stay intact.
    A due yield is delayed by at most the remaining31 pixels of that run.
    """
    total=0
    def function(m):
        nonlocal total
        body=m[0];changed=0
        for pc in edges:
            pattern=(rb'    RECOMP_BACKEDGE\(\);\r?\n'
                     rb'(    if \([^\r\n]+goto L_[0-9A-F]+; /\* 0x%08X:[^\r\n]+\*/)' % pc)
            def replace(edge):
                return (b'    ++_display_rle_budget;\n'+edge[1]+
                        b'\n    { uint32_t _display_edges=_display_rle_budget; _display_rle_budget=0; '
                        b'if(!recomp_yield_hook || (uint64_t)g_backedges+_display_edges<RECOMP_YIELD_EVERY) '
                        b'g_backedges+=_display_edges; else while(_display_edges--){ RECOMP_BACKEDGE(); } }')
            body,n=re.subn(pattern,replace,body)
            if n:seen.add(f'{module}:{label}{pc:08X}');changed+=n
        if changed:
            body=body.replace(b'{',b'{\n    uint32_t _display_rle_budget=0;',1)
            total+=changed
        return body
    text=re.sub(rb'(?ms)^void sub_[A-Za-z0-9_]+\(void\) \{.*?^\}',function,text)
    return text,total

def batch_rle_backedges(text,module):
    return batch_run_backedges(text,module,RLE_BACKEDGES.get(module,()),'rle')


def batch_opaque_backedges(text,module):
    return batch_run_backedges(text,module,tuple(edge for _,edge in OPAQUE_RUNS.get(module,())),'opaque-edge')


def required(module):
    p=LAYOUT.get(module)
    terrain={f'{module}:terrain-copy'} if module in TERRAIN_COPIES else set()
    fill={f'{module}:fill-row'} if module in FILL_ROWS else set()
    commands={f'{module}:half-commands{address:08X}'
              for address in (RLE_STACK_FRAME[module],HALF_TGX_ENTRY[module])} if module in RLE_STACK_FRAME else set()
    commands.update(f'{module}:half-palette{address:08X}' for address in HALF_PALETTE.get(module,(0,()))[1])
    if p is None:return (terrain|fill|commands|{f'{module}:rle{pc:08X}' for pc in RLE_BACKEDGES.get(module,())}|
        {f'{module}:half{pc:08X}' for pc,_,_ in HALF_RUNS.get(module,())}|
        {f'{module}:opaque{pc:08X}' for pc,_ in OPAQUE_RUNS.get(module,())}|
        {f'{module}:opaque-edge{pc:08X}' for _,pc in OPAQUE_RUNS.get(module,())})
    return ({f'{module}:{v:08X}' for v in (p['frame'],p['world_call'],p['after'],
                                         *p['groups'],p['overlay'],p['present_call'],p['clear_clip'],*p['clear_calls'])}|
            {f'{module}:rle{v:08X}' for v in RLE_BACKEDGES[module]}|
            {f'{module}:half{pc:08X}' for pc,_,_ in HALF_RUNS[module]}|
            {f'{module}:preview{PREVIEW_STORES[module]:08X}',f'{module}:resource-animation',
             f'{module}:{PROGRESS[module][0]:08X}'}|
            {f'{module}:{pc:08X}' for pc in SAVE_PRESENTS[module]}|
            {f'{module}:opaque{pc:08X}' for pc,_ in OPAQUE_RUNS[module]}|
            {f'{module}:opaque-edge{pc:08X}' for _,pc in OPAQUE_RUNS[module]}|
            {f'{module}:terrain-raster',f'{module}:hud-literal64',f'{module}:hud-keyed'}|
            {f'{module}:hud-span{pc:08X}' for pc in HUD_SPANS[module]}|
            {f'{module}:bar{pc:08X}' for pc in BARS[module]}|
            {f'{module}:palette-fade{PALETTE_FADE[module][0]+offset:08X}'
             for offset in PALETTE_FADE_STORES+PALETTE_FADE_INTERMEDIATE}|
            {f'{module}:crest-fade{pc:08X}' for pc in CREST_FADE_STORES[module]})|terrain|fill|commands

def apply(text,module):
    text,count=collapse_terrain_copies(text,module)
    text,batched=collapse_fill_row(text,module);count+=batched
    text,batched=collapse_half_runs(text,module);count+=batched
    text,batched=native_half_commands(text,module);count+=batched
    text,batched=native_half_commands(text,module,palette=True);count+=batched
    text,batched=batch_rle_backedges(text,module);count+=batched
    p=LAYOUT.get(module)
    if p is None:return text,count
    text,batched=batch_opaque_backedges(text,module);count+=batched
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
        if raster_stack_store(line,pc,module):return line
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
        palette_start,palette_table=PALETTE_FADE[module]
        palette_offset=pc-palette_start
        if palette_offset in PALETTE_FADE_INTERMEDIATE:
            seen.add(f'{module}:palette-fade{pc:08X}');return line
        original_pc=pc+(0x240 if module=='classic' and 0x44E830<=pc<0x44F8E0 else 0)
        # GM1 blending has an intermediate source write followed by the final
        # source+destination sum. Publish coverage once, after the final sum.
        if original_pc in (0x44ECF0,0x44F16C,0x44F3E9,0x44F87F):return line
        background=None
        if pc in CREST_FADE_STORES[module]:
            # Custom player crests use arithmetic fade, with its amount retained in ESI.
            background=b'(esi*255u/32u)'
            seen.add(f'{module}:crest-fade{pc:08X}')
        elif palette_offset in PALETTE_FADE_STORES:
            # Paletted crest fade also reuses +1C for pitch; -14 keeps its forward lookup.
            background=b'((MEM32(ebp-0x14u)-0x%Xu)/512u*255u/32u)' % palette_table
            seen.add(f'{module}:palette-fade{pc:08X}')
        elif 0x44EA70<=original_pc<0x44F460:
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
    text,literal=batch_hud_literal64(text,module)
    count+=literal
    text,spans=batch_hud_spans(text,module)
    count+=spans
    text,keyed=batch_hud_keyed(text,module)
    count+=keyed
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
        if literal:declarations+=b'\nextern int stronghold_display_hud_literal64(uint32_t,uint32_t);'
        if spans:
            declarations+=(b'\nextern int stronghold_display_hud_literal_span(uint32_t,uint32_t,unsigned);'
                           b'\nextern int stronghold_display_hud_solid_span(uint32_t,uint32_t,unsigned);')
        if keyed:declarations+=b'\nextern int stronghold_display_hud_keyed(uint32_t,uint32_t,unsigned,uint16_t);'
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
