"""SHA-pinned original text editors plus optional production ARM64 bridge checks."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess

import capstone
import pefile
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32
from unicorn.x86_const import UC_X86_REG_EAX, UC_X86_REG_ESP, UC_X86_REG_EIP
from test_touch import Original
from build_android import CLANG, ROOT
import original_hooks

LAYOUT={
    'stronghold':dict(name='Stronghold.exe',editor=0xEC64F0,clear=0x461860,char=0x462AE0,width=0x4605E0,
                      draws={0x47C960:0x2D,0x46AEF0:0x18,0x49D000:0x1E}),
    'classic':dict(name='Stronghold Crusader.exe',editor=0x17893D0,clear=0x473510,char=0x474870,width=0x472250,
                   draws={0x4939C0:0x2D,0x47D7E0:0x18}),
    'extreme':dict(name='Stronghold_Crusader_Extreme.exe',editor=0x20BC3F8,clear=0x473730,char=0x474A90,width=0x472460,
                   draws={0x493AE0:0x2D,0x47D9D0:0x18})}


class Editor(Original):
    def __init__(self,edition):
        self.d=LAYOUT[edition];self.edition=edition
        raw=(ROOT/'generated/input'/self.d['name']).read_bytes();self.sha=hashlib.sha256(raw).hexdigest()
        assert self.sha==json.loads((ROOT/'config/inputs.json').read_text())[self.d['name']]
        self.pe=pefile.PE(data=raw);image=self.pe.get_memory_mapped_image();self.u=Uc(UC_ARCH_X86,UC_MODE_32)
        self.u.mem_map(0x400000,(len(image)+4095)&~4095);self.u.mem_write(0x400000,image)
        self.stack,self.end=0x10008000,0x20000000;self.u.mem_map(0x10000000,0x20000);self.u.mem_map(self.end,4096)

    def execute(self):
        md=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_32);md.detail=True
        for draw,offset in self.d['draws'].items():
            i=next(md.disasm(bytes(self.u.mem_read(draw+offset,8)),draw+offset))
            assert i.mnemonic=='mov' and i.operands[1].imm==self.d['editor'],(self.edition,draw,i.op_str)
        source=(ROOT/'android/host/game_text.c').read_text().upper()
        for address in (self.d['editor'],self.d['clear'],self.d['char'],*self.d['draws']):assert f'0X{address:X}U' in source
        result=[]
        for bank,text,limit,expected in [(0,'Тестовый',4,'Тест'),(5,'A1.2/3.4B',20,'1.23.4'),(6,'12.3A',20,'123')]:
            editor=self.d['editor'];self.w(editor,bank);self.w(editor+80+4*bank,limit);self.w(editor+272+4*bank,1000)
            self.run(self.d['clear'],this=editor)
            def font_width(pc):
                if pc==self.d['width']:
                    # Only the glyph-width leaf needs unavailable font assets;
                    # the real bank filter, cursor/length cap and writes run.
                    sp=self.u.reg_read(UC_X86_REG_ESP);self.u.reg_write(UC_X86_REG_EAX,0)
                    self.u.reg_write(UC_X86_REG_ESP,sp+12);self.u.reg_write(UC_X86_REG_EIP,self.r(sp))
            for char in text:self.run(self.d['char'],(ord(char),),editor,hook=font_width)
            length=self.r(editor+144+4*bank)
            actual=bytes(self.u.mem_read(editor+336+500*bank,2*length)).decode('utf-16le')
            assert actual==expected,(self.edition,bank,actual,expected)
            result.append(dict(bank=bank,text=actual,limit=limit,length=length))
        if self.edition=='stronghold':
            assert bytes(self.u.mem_read(0x45A780,2))==b'\x89\x01'
            for pc in (0x4BC0D5,0x4BC160):assert bytes(self.u.mem_read(pc,2))==b'\xff\xd2'
            for name in ('recomp_0020.c','recomp_0040.c'):
                patched,_=original_hooks.apply((ROOT/'generated/stronghold-x86'/name).read_bytes(),'stronghold')
                if name=='recomp_0020.c':assert b'stronghold_text_focus(ecx); MEM32(ecx) = eax;' in patched
                else:assert patched.count(b'stronghold_game_ui_click(esi); stronghold_original_build_click(esi); RECOMP_ICALL(edx);')>=2
        return dict(sha256=self.sha,scope='Real original clear/character/filter/limit code; glyph width only stubbed. Widget editor-object operands and HD hook bytes verified.',cases=result)


def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--serial');args=parser.parse_args()
    result={edition:Editor(edition).execute() for edition in LAYOUT}
    for edition in LAYOUT:
        if not args.serial:continue
        binary=ROOT/f'build/game-text-{edition}-selfcheck'
        flags={'stronghold':['-DSTRONGHOLD_ORIGINAL'],'classic':['-DSTRONGHOLD_CLASSIC'],'extreme':[]}[edition]
        subprocess.run([str(CLANG),'--target=aarch64-linux-android30','-O2','-std=gnu11',*flags,
                        '-Iandroid/runtime','-Iandroid/host','tests/game_text_selfcheck.c','-o',str(binary),
                        '-Wl,-z,max-page-size=16384'],cwd=ROOT,check=True)
        target=f'/data/local/tmp/game-text-{edition}-selfcheck';adb=['adb','-s',args.serial]
        subprocess.run([*adb,'push',str(binary),target],capture_output=True,check=True)
        subprocess.run([*adb,'shell','chmod','755',target],capture_output=True,check=True)
        run=subprocess.run([*adb,'shell',target],capture_output=True,text=True,check=True)
        assert 'PASS' in run.stdout;result[edition]['native']=run.stdout.strip()
        subprocess.run([*adb,'shell','rm',target],capture_output=True,check=True)
    (ROOT/'generated/game-text-original-proof.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result,indent=2))


if __name__=='__main__':main()
