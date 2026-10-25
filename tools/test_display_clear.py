"""Execute original fill/clipper and prove the batching rectangle is exact."""
import hashlib
import argparse
import json
import struct
import subprocess
from pathlib import Path

from unicorn import UC_HOOK_CODE, UC_HOOK_MEM_WRITE
from unicorn.x86_const import UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_ESP
from test_display import Original, ROOT
import display_hooks


def cases(module):
    o = Original(module)
    u = o.u
    classic = module == 'classic'
    context = 0x1A55350 if classic else 0x248C3A0
    screen = 0xFD5780 if classic else 0xFD5BE0
    function = 0x471430 if classic else 0x471640
    clipping = display_hooks.LAYOUT[module]['clear_clip'] + 5
    rectangles = [(0, 2, 5, 4), (-5, -3, 5, 4), (5, 4, -5, -3),
                  (15, 9, 20, 15), (20, 15, 15, 9), (3, 5, 3, 5),
                  (-6, 3, -2, 7), (4060, 3, 4090, 7), (3, -6, 8, -2),
                  (3, 12, 8, 20), (4, 11, 1, 0), (15, 9, 16, 10)]
    result = []
    for offscreen in (False, True):
        pitch = 8112 if offscreen else 32
        base = o.dst
        size = pitch * 12
        o.w(screen + 0x38, 16)
        o.w(screen + 0x3C, 12)
        o.w(screen + 0x40, 32)
        o.w(screen + 0xD4, base)
        o.w(screen + 0xD8, base)
        o.w(context + 0xC, int(offscreen))
        for offset in (0x58, 0x60):
            o.w(o.renderer + offset, 2)
            o.w(o.renderer + offset + 4, 10)
        for rectangle in rectangles:
            u.mem_write(base - 16, b'\xA5' * (size + 32))
            stores = []
            clipped = []

            def at_clip(uc, address, length, user):
                good = uc.reg_read(UC_X86_REG_EAX) != 0
                geometry = tuple(o.r(context + offset) for offset in (0x30, 0x34, 0x38, 0x3C))
                clipped.append((good, geometry))

            def at_store(uc, access, address, width, value, user):
                if base - 16 <= address < base + size + 16:
                    assert width == 2 and value == 0xF81F
                    stores.append(address)

            code = u.hook_add(UC_HOOK_CODE, at_clip, begin=clipping, end=clipping)
            memory = u.hook_add(UC_HOOK_MEM_WRITE, at_store)
            u.mem_write(o.sp, struct.pack('<6I', 0x11000000, *[n & 0xFFFFFFFF for n in rectangle], 0xF81F))
            u.reg_write(UC_X86_REG_ECX, context)
            u.reg_write(UC_X86_REG_ESP, o.sp)
            u.emu_start(function, 0x11000000, count=400000)
            u.hook_del(code)
            u.hook_del(memory)
            assert len(clipped) == 1
            good, (left, top, right, bottom) = clipped[0]
            expected = []
            framebuffer = bytearray(b'\xA5' * size)
            if good:
                for y in range(top, bottom + 1):
                    for x in range(left, right + 1):
                        expected.append(base + pitch * y + x * 2)
                        struct.pack_into('<H', framebuffer, pitch * y + x * 2, 0xF81F)
            assert stores == expected, (module, offscreen, rectangle, clipped, len(stores), len(expected))
            assert bytes(u.mem_read(base, size)) == framebuffer
            assert bytes(u.mem_read(base - 16, 16)) == b'\xA5' * 16
            assert bytes(u.mem_read(base + size, 16)) == b'\xA5' * 16
            if good:
                # The row loop mutates top and destroys height after our seam.
                assert o.r(context + 0x34) == bottom + 1
                assert o.r(context + 0x2C) == 0xFFFFFFFF
            result.append(dict(offscreen=offscreen, rectangle=rectangle, success=good,
                               clipped=[left, top, right, bottom] if good else None,
                               performed_pixel_stores=len(stores)))
    return dict(edition=module, input_sha256=o.sha256, cases=result)


if __name__ == '__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native-device',help='Also build/run the isolated display C selfcheck; no UI input.')
    args=parser.parse_args()
    result = dict(status='PASS', cases=[cases(module) for module in ('classic', 'game')],
                  hook_sha256=hashlib.sha256((ROOT / 'tools/display_hooks.py').read_bytes()).hexdigest(),
                  note='The original clipper supplies normalized inclusive bounds; reverse endpoints, partial clip and no-write failures are exercised.')
    if args.native_device:
        from build_android import CLANG
        from verify_elf_pages import inspect
        adb=Path.home()/'AppData/Local/Android/Sdk/platform-tools/adb.exe'
        result['native']=[]
        for edition in ('game','classic'):
            binary=ROOT/f'build/display-clear-{edition}-selfcheck'
            command=[str(CLANG),'--target=aarch64-linux-android30','-O2','-std=gnu11','-fno-strict-aliasing','-fwrapv',
                     '-Iandroid/runtime','-Iandroid/host','-DSTRONGHOLD_DISPLAY_SELFTEST']
            if edition=='classic':command+=['-DSTRONGHOLD_CLASSIC']
            command+=['android/host/display.c','-o',str(binary),'-Wl,-z,max-page-size=16384','-Wl,-z,common-page-size=16384','-lm']
            subprocess.run(command,check=True,cwd=ROOT)
            elf=inspect(binary.read_bytes())
            destination='/data/local/tmp/display-clear-'+edition+'-selfcheck'
            prefix=[str(adb),'-s',args.native_device]
            subprocess.run([*prefix,'push',str(binary),destination],check=True,capture_output=True)
            subprocess.run([*prefix,'shell','chmod','755',destination],check=True,capture_output=True)
            run=subprocess.run([*prefix,'shell',destination],check=True,capture_output=True,text=True)
            assert 'batched semantic clear:' in run.stdout and 'PASS' in run.stdout
            result['native'].append(dict(edition=edition,path=str(binary.relative_to(ROOT)),
                                         sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),device=args.native_device,
                                         exit_code=run.returncode,stdout=run.stdout,elf=elf))
    (ROOT / 'generated/display-clear-original.json').write_text(json.dumps(result, indent=2) + '\n')
    print('Original x86 clear rectangle:48 cases/two editions, D4/D8, reversed/partial/full clipping, exact stores/canaries PASS')
