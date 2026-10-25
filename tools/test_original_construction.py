"""SHA-pinned HD construction callbacks and optional production ARM64 bridge."""
import argparse
import json
import subprocess
from unicorn.x86_const import UC_X86_REG_EAX, UC_X86_REG_ESP, UC_X86_REG_EIP, UC_X86_REG_ECX
from test_game_text import Editor
from build_android import ROOT, CLANG
import original_hooks


def proof():
    original=Editor('stronghold');u=original.u
    def ret(value=0,pop=0):
        sp=u.reg_read(UC_X86_REG_ESP);u.reg_write(UC_X86_REG_EAX,value)
        u.reg_write(UC_X86_REG_EIP,original.r(sp));u.reg_write(UC_X86_REG_ESP,sp+4+pop)
    # The real section callback clears construction and queues the original
    # state14/subsection request; only selection, state setter and sound leaves
    # are intercepted, so no rendering assets or Windows runtime are needed.
    requests=[]
    def section(pc):
        if pc in(0x4F7C50,0x4F9490):ret()
        elif pc==0x43E790:ret(pop=4)
        elif pc==0x45BEB0:
            sp=u.reg_read(UC_X86_REG_ESP)
            requests.append((u.reg_read(UC_X86_REG_ECX),original.r(sp+4),original.r(sp+8)));ret(pop=8)
    section_cases=[]
    for tab in(10,20,40,30,28,25,26,27,29):
        original.w(0x1503D38,14);original.w(0x1503D3C,11 if tab==10 else 10)
        original.w(0x1503B24,13);original.w(0x1008144,1);original.w(0xD4716C,0)
        requests.clear();original.run(0x43ADA0,(tab,),hook=section)
        assert original.r(0x1503D50)==tab and original.r(0x1503B24)==0
        assert requests==[(0x1503D30,14,0)] and u.reg_read(UC_X86_REG_ESP)==original.stack+4
        section_cases.append(dict(tab=tab,request_state=14,original_cdecl_stack='PASS'))
    # Real tab drawer: editor-only tabs and initial economy are gated in the
    # game, rather than inferred from Crusader. Stop at its raster draw leaf.
    tab_cases=[]
    for mode,economy,tab,disabled in((0,0,20,1),(0,1,20,0),(0,1,26,1),(1,0,26,0),(6,0,27,0)):
        original.w(0x1503D38,14);original.w(0x1503D3C,10);original.w(0x1503D84,mode)
        original.w(0x1008144,1);original.w(0xF671D0,0)
        for field in(30037,30057):original.w(0xBCAFB8+4*(field+1494),economy)
        original.w(0xBCAFB8+4*43447,0);original.w(0xA656F4,0)
        def drawer(pc):
            if pc==0x455130:ret(pop=4)
            elif pc==0x448880:ret(pop=16)
        original.run(0x456070,(tab,),hook=drawer)
        assert original.r(0xA65704)==disabled,(mode,economy,tab,original.r(0xA65704))
        tab_cases.append(dict(mode=mode,economy=economy,tab=tab,disabled=disabled))
    # Original tooltip-table dispatch proves the preserved description/cost
    # request, including the original disabled-help behavior.
    node,owner=0x10010000,0x10011000
    indices=[i for i in range(1,1000) if original.r(0x5BBFA8+i*16)==3]
    assert indices
    index=indices[0];original.h(node+44,index);original.w(owner+56,node)
    original.w(0x1503D38,14);original.w(0x821CF0,original.end+16)
    # Resolve the actual imported clock operand from the original call.
    clock=bytes(u.mem_read(0x4BDDF5,6));assert clock[:2]==b'\xff\x15'
    clockslot=int.from_bytes(clock[2:6],'little');original.w(clockslot,original.end+16)
    descriptions=[]
    for native in(False,True):
        original.w(0x1503DE8,0);descriptions.clear()
        def tooltip(pc):
            if pc==original.end+16:ret(1000)
            elif pc==0x4BDEB6 and native:
                # Exact helper effect: HELP=1 without changing the guest flag.
                u.reg_write(UC_X86_REG_EIP,0x4BDEB8)
            elif pc==0x4BCA30:
                sp=u.reg_read(UC_X86_REG_ESP);descriptions.append([original.r(sp+4+i*4) for i in range(6)]);ret(pop=24)
        original.run(0x4BDDF0,this=owner,hook=tooltip)
        expected=[2,*[original.r(0x5BBFAC+index*16+i*4) for i in range(3)],50,0xFFFFFFFF]
        assert descriptions==([expected] if native else []),(native,descriptions,expected)
        assert original.r(0x1503DE8)==0
    original_hooks.seen.clear()
    sources={}
    for path in (ROOT/'generated/stronghold-x86').glob('recomp_*.c'):
        patched,_=original_hooks.apply(path.read_bytes(),'stronghold')
        if path.name in('recomp_0040.c','recomp_0058.c'):sources[path.name]=patched
    assert not(original_hooks.required('stronghold')-original_hooks.seen)
    assert b'stronghold_original_build_draw(ecx); esi = ecx;' in sources['recomp_0040.c']
    assert b'stronghold_touch_ui_help(MEM32(0x1503DE8))' in sources['recomp_0040.c']
    assert b'if (stronghold_touch_ui_redraw()) goto L_005330D3;' in sources['recomp_0058.c']
    return dict(sha256=original.sha,scope='Real original section stores/cdecl return, tab availability drawer, original tooltip-table description/cost dispatch; selection/state/raster/sound/clock leaves stubbed.',section_cases=section_cases,tab_cases=tab_cases,tooltip_args=expected,all_generated_seams='PASS')


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--serial');args=parser.parse_args();result=proof()
    if args.serial:
        binary=ROOT/'build/original-construction-selfcheck'
        subprocess.run([str(CLANG),'--target=aarch64-linux-android30','-DSTRONGHOLD_ORIGINAL','-O2',
            '-Iandroid/runtime','-Iandroid/host','tests/original_construction_selfcheck.c','-o',str(binary),
            '-Wl,-z,max-page-size=16384'],cwd=ROOT,check=True)
        target='/data/local/tmp/original-construction-selfcheck';adb=['adb','-s',args.serial]
        subprocess.run([*adb,'push',str(binary),target],capture_output=True,check=True)
        subprocess.run([*adb,'shell','chmod','755',target],capture_output=True,check=True)
        run=subprocess.run([*adb,'shell',target],capture_output=True,text=True,check=True)
        assert 'PASS' in run.stdout;result['native']=run.stdout.strip()
        subprocess.run([*adb,'shell','rm',target],capture_output=True,check=True)
    (ROOT/'generated/original-construction-proof.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result,indent=2))


if __name__=='__main__':main()
