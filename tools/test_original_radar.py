"""Execute original HD team mask and radar palette leaves for both pixel formats."""
import json
from unicorn.x86_const import UC_X86_REG_EAX,UC_X86_REG_ECX,UC_X86_REG_EBP,UC_X86_REG_EIP
from test_game_text import Editor
from build_android import ROOT
import original_hooks


def main():
    e=Editor('stronghold');u=e.u;teams=[0,7,7,8,8,9,10,11,12]
    for player,team in enumerate(teams):e.w(0xBF6424+4*player,team)
    e.h(0xD47476+1044*2,1)
    mask=e.run(0x44C100,(2,))
    assert mask==0xFC,hex(mask) # The original excludes both matching-team owners.
    frame,obj=0x10018000,0x10019000
    cases=[]
    for owner in(1,2,3,8):
        for native in(False,True):
            mapped=(2 if mask&(1<<(owner-1)) else 8) if native else e.r(0x7A6924+4*owner)
            for phase in range(4):
                for fmt in range(2):
                    for flashing in(False,True):
                        u.reg_write(UC_X86_REG_EBP,frame);e.w(frame-0x54,obj);e.w(frame-0x1C,fmt);e.w(obj+64,phase)
                        pc=0x49574A if flashing else 0x49576E
                        def leaf(address):
                            if address==pc and native:
                                # Production helper is covered by the ARM64 control
                                # selfcheck. Its index enters the unmodified x86
                                # format/phase arithmetic at exactly this load.
                                u.reg_write(UC_X86_REG_EAX if flashing else UC_X86_REG_ECX,mapped)
                                u.reg_write(UC_X86_REG_EIP,pc+7)
                            elif address==0x495786:u.emu_stop()
                        e.run(pc,this=owner,hook=leaf)
                        actual=e.r(frame+0x1C)
                        expected=e.rh(0x7EA240+2*(mapped*8+(phase*2 if flashing else 0)+fmt))
                        assert actual==expected,(owner,native,phase,fmt,flashing,actual,expected)
                        cases.append(dict(owner=owner,native=native,phase=phase,format=fmt,flashing=flashing,color=actual))
    # Original neutral branch never enters either remapped owner lookup.
    e.h(0xD47476+1044*2,0)
    for fmt in range(2):
        e.w(frame-0x1C,fmt);u.reg_write(UC_X86_REG_EBP,frame);u.reg_write(UC_X86_REG_EAX,2)
        def neutral(pc):
            if pc in(0x49574A,0x49576E):raise AssertionError('neutral entered player-colour lookup')
            if pc==0x495786:u.emu_stop()
        e.run(0x4956C0,hook=neutral)
        assert e.r(frame+0x1C)==e.rh(0x7EA248+2*fmt)
    patched,_=original_hooks.apply((ROOT/'generated/stronghold-x86/recomp_0033.c').read_bytes(),'stronghold')
    for register in(b'eax',b'ecx'):
        assert register+b' = stronghold_original_radar_color(ecx, MEM32(ecx * 4 + 0x7A6924));' in patched
    result=dict(sha256=e.sha,scope='Real original enemy-team mask44C100 and all original ordinary/flashing palette arithmetic; native palette-index result injected at the two tested production seams.',
                original_enemy_mask=mask,cases=len(cases),formats=['RGB555','RGB565'],phases=4,
                green=[hex(e.rh(0x7EA240+2*(8*8+i))) for i in range(8)],
                red=[hex(e.rh(0x7EA240+2*(2*8+i))) for i in range(8)],neutral_branch='unchanged PASS',seams='PASS')
    (ROOT/'generated/original-radar-proof.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))


if __name__=='__main__':main()
