"""Run HD's real tinted TGX mask: colour and opacity travel in separate inputs."""
import struct
from unicorn import UC_HOOK_MEM_WRITE
from unicorn.x86_const import UC_X86_REG_EIP, UC_X86_REG_EDX
from test_game_text import Editor
from test_display_alpha import compose


def main():
    cases = 0
    for repeat in (False, True):
        for fade in (0, 8, 16, 24, 32):
            original = Editor('stronghold'); u = original.u
            dst, source, renderer = 0x10011000, 0x10011200, 0x10011400
            original.w(0xB183A0, dst); original.w(0xB1831C, 0x565)
            original.w(renderer+4, 1); original.w(renderer+0x58, 0); original.w(renderer+0x5C, 20)
            pixels = [0, 0xFFFF, 0xF81F, 0x7BEF]
            u.mem_write(source, (b'\x43'+struct.pack('<H',0x7BEF) if repeat else b'\x03'+struct.pack('<4H',*pixels))+b'\x80')
            alpha = [0]*4

            def written(machine, access, address, size, value, user):
                if not dst <= address < dst+8:
                    return
                pc = machine.reg_read(UC_X86_REG_EIP)
                remaining = (32-(machine.reg_read(UC_X86_REG_EDX)&65535))*255//32 if pc in (0x442FA4,0x4430B1) else 0
                x = (address-dst)//2
                alpha[x] = 255-((255-alpha[x])*remaining+127)//255

            def run(background, capture):
                u.mem_write(dst,struct.pack('<4H',*([background]*4)))
                hook = u.hook_add(UC_HOOK_MEM_WRITE,written) if capture else None
                original.run(0x442D50,(0,0,4,1,source,0x7BEF,fade),renderer)
                if hook is not None: u.hook_del(hook)
                return struct.unpack('<4H',u.mem_read(dst,8))

            captured = run(0xF81F,True)
            for background in (0,0xFFFF,0x07FF,0xF800):
                expected = run(background,False)
                for x in range(4):
                    actual = compose(captured[x],background,alpha[x])
                    for shift,mask in ((0,31),(5,63),(11,31)):
                        assert abs(((actual>>shift)&mask)-((expected[x]>>shift)&mask))<=2,(repeat,fade,x,hex(actual),hex(expected[x]),alpha)
                cases += 1
    print(f'HD tinted dialog/font mask: {cases} real x86 literal/repeat/fade/background cases, RGB565 composition within two channel units PASS')


if __name__ == '__main__': main()
