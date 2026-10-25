"""Check UI coverage/alpha against performed stores in both original x86 PEs."""
import json
import struct
import re
from pathlib import Path

from unicorn import UC_HOOK_MEM_WRITE
from unicorn.x86_const import UC_X86_REG_ECX, UC_X86_REG_ESP, UC_X86_REG_EIP, UC_X86_REG_EBP
from test_display import Original, ROOT
import display_hooks

KEY = 0xF81F
INTERMEDIATE = {0x44ECF0, 0x44F16C, 0x44F3E9, 0x44F87F}
VARIABLE = {0x44F7EC, 0x44F8D5, 0x44FA45, 0x44FAFE}


def compose(pixel, below, alpha):
    out = 0
    for shift, maximum in ((0, 31), (5, 63), (11, 31)):
        p, b, k = ((v >> shift) & maximum for v in (pixel, below, KEY))
        # C99 integer division truncates toward zero.
        n = int((p * 255 + (b - k) * (255 - alpha) + 127) / 255)
        out |= max(0, min(maximum, n)) << shift
    return out


def cases(module):
    o = Original(module)
    u = o.u
    key_instruction = 0x45543B if module == "game" else 0x4551FB
    raw = bytes(u.mem_read(key_instruction, 7))
    assert raw[:3] == b"\x66\x8b\x1d"  # original mov bx, word ptr [absolute key]
    key_address = struct.unpack_from("<I", raw, 3)[0]
    defines = re.findall(r"#define UI_COLOR_KEY 0x([0-9A-Fa-f]+)u", (ROOT / "android/host/display.c").read_text())
    assert int(defines[0 if module == "classic" else 1], 16) == key_address
    delta = 0 if module == "game" else 0x240
    screen = 0xFD5BE0 if module == "game" else 0xFD5780
    mask_va = 0xF6A064 if module == "game" else 0xF69C04
    mask_width = 0xF6A070 if module == "game" else 0xF69C10
    o.w(screen + 0xD8, o.dst)
    o.w(screen + 0x54, 0x565)
    o.w(o.renderer + 4, 1)
    o.w(o.renderer + 0x58, 0)
    o.w(o.renderer + 0x5C, 20)
    o.w(mask_va, o.tgx + 0x1000)
    o.w(mask_width, 4)
    # Three opaque literals, then a skipped pixel: identical RGB values are
    # insufficient to distinguish the opaque key-colored pixel from the hole.
    u.mem_write(o.tgx, b"\x02" + struct.pack("<3H", 0, 0x07FF, KEY) + b"\x20\x80")
    u.mem_write(o.tgx + 0x1000, struct.pack("<4H", 31, 16, 1, 0))
    results = []
    for function in (0x44EA70, 0x44F5B0):
        for amount in ((0, 8, 16, 24, 32) if function == 0x44EA70 else (0, 5, 15)):
            alpha = [0] * 4
            stores = []

            def written(uc, access, address, size, value, user):
                if not o.dst <= address < o.dst + 8:
                    return
                pc = uc.reg_read(UC_X86_REG_EIP) + delta
                if pc in INTERMEDIATE:
                    return
                x = (address - o.dst) // 2
                remaining = 0
                if function == 0x44EA70:
                    bp = uc.reg_read(UC_X86_REG_EBP)
                    base = 0xD7F4A0 if module == 'game' else 0xD7F320
                    saved_amount = (o.r(bp - 0xC) - base) // 512
                    assert saved_amount == amount
                    # The original repurposes the public argument for pitch.
                    assert o.r(bp + 0x1C) != amount
                    remaining = saved_amount * 255 // 32
                elif pc in VARIABLE:
                    bp = uc.reg_read(UC_X86_REG_EBP)
                    at = o.r(bp - 4) - 2
                    source_alpha = struct.unpack("<H", u.mem_read(at, 2))[0] & 31
                    assert source_alpha == (31, 16, 1, 0)[x]
                    remaining = (32 - (source_alpha - amount)) * 255 // 32
                alpha[x] = 255 - ((255 - alpha[x]) * remaining + 127) // 255
                stores.append(pc)

            def run(background, track):
                u.mem_write(o.dst, struct.pack("<4H", *([background] * 4)))
                u.mem_write(o.sp, struct.pack("<7I", 0x11000000, 0, 0, 4, 1, o.tgx, amount))
                u.reg_write(UC_X86_REG_ECX, o.renderer)
                u.reg_write(UC_X86_REG_ESP, o.sp)
                hook = u.hook_add(UC_HOOK_MEM_WRITE, written) if track else None
                u.emu_start(function - delta, 0x11000000, count=200000)
                if hook is not None:
                    u.hook_del(hook)
                return struct.unpack("<4H", u.mem_read(o.dst, 8))

            captured = run(KEY, True)
            assert alpha[3] == 0
            if function == 0x44EA70 and amount == 0:
                assert alpha == [255, 255, 255, 0]
                assert captured[0] == 0 and captured[2] == KEY
            maximum_error = 0
            for background in (0, 0xFFFF, 0x07FF, 0xF800):
                original = run(background, False)
                for x in range(4):
                    actual = compose(captured[x], background, alpha[x])
                    for shift, limit in ((0, 31), (5, 63), (11, 31)):
                        error = abs(((actual >> shift) & limit) - ((original[x] >> shift) & limit))
                        maximum_error = max(maximum_error, error)
                        assert error <= 2, (module, hex(function), amount, x, hex(actual), hex(original[x]), alpha)
            # The real generated-store hooks must publish the corresponding
            # blend operation, including intermediate-write suppression.
            source = (b'#include "recomp_funcs.h"\n    MEM16(edi) = (uint16_t)(LO16(eax)); /* 0x%08X: mov word ptr [edi], ax */\n'
                      % (stores[0] - delta))
            patched, count = display_hooks.apply(source, module)
            assert count and b"stronghold_display_" in patched
            if function == 0x44EA70:
                assert b'MEM32(ebp-0xcu)' in patched and b'MEM32(ebp+0x1cu)' not in patched
            results.append(dict(function=hex(function - delta), amount=amount, alpha=alpha,
                                final_stores=[hex(p - delta) for p in stores], max_rgb565_channel_error=maximum_error))
    return dict(edition=module, input_sha256=o.sha256, original_key_address=hex(key_address), cases=results)


def half_constant_cases(module):
    o = Original(module)
    u = o.u
    delta = 0 if module == 'game' else 0x240
    screen = 0xFD5BE0 if module == 'game' else 0xFD5780
    base = 0xD7F4A0 if module == 'game' else 0xD7F320
    o.w(screen + 0xD8, o.dst)
    o.w(screen + 0x54, 0x565)
    o.w(o.renderer + 4, 1)
    o.w(o.renderer + 0x54, 1)
    o.w(o.renderer + 0x58, 0)
    o.w(o.renderer + 0x5C, 2)
    results = []
    for repeat in (False, True):
        colors = [0x2104, 0x07FF, KEY, 0xFFFF, 0, 0x1111, 0xF800, 0x07E0]
        data = bytes([0x47 if repeat else 7]) + struct.pack('<' + 'H' * (1 if repeat else 8), *(colors[:1] if repeat else colors)) + b'\x80'
        u.mem_write(o.tgx, data)
        for amount in (0, 8, 16, 24, 32):
            alpha = [0] * 4
            stores = []

            def written(uc, access, address, size, value, user):
                pc = uc.reg_read(UC_X86_REG_EIP) + delta
                if pc in INTERMEDIATE or not o.dst <= address < o.dst + 8:
                    return
                bp = uc.reg_read(UC_X86_REG_EBP)
                derived = (o.r(bp - 0xC) - base) // 512
                assert derived == amount and o.r(bp + 0x1C) == 8112
                x = (address - o.dst) // 2
                remaining = derived * 255 // 32
                alpha[x] = 255 - ((255 - alpha[x]) * remaining + 127) // 255
                stores.append(pc - delta)

            def run(background, track):
                u.mem_write(o.dst, struct.pack('<4H', *([background] * 4)))
                u.mem_write(o.sp, struct.pack('<7I', 0x11000000, 0, 0, 8, 1, o.tgx, amount))
                u.reg_write(UC_X86_REG_ECX, o.renderer)
                u.reg_write(UC_X86_REG_ESP, o.sp)
                hook = u.hook_add(UC_HOOK_MEM_WRITE, written) if track else None
                u.emu_start(0x44EA70 - delta, 0x11000000, count=200000)
                if hook is not None:
                    u.hook_del(hook)
                return struct.unpack('<4H', u.mem_read(o.dst, 8))

            captured = run(KEY, True)
            assert len(stores) == 4 and (amount != 0 or alpha == [255] * 4)
            maximum_error = 0
            for background in (0, 0xFFFF, 0x07FF, 0xF800):
                expected = run(background, False)
                for x in range(4):
                    actual = compose(captured[x], background, alpha[x])
                    for shift, limit in ((0, 31), (5, 63), (11, 31)):
                        error = abs(((actual >> shift) & limit) - ((expected[x] >> shift) & limit))
                        maximum_error = max(error, maximum_error)
                        assert error <= 2, (module, repeat, amount, x, hex(actual), hex(expected[x]), alpha)
            results.append(dict(repeat=repeat, amount=amount, alpha=alpha, final_stores=[hex(p) for p in stores], max_rgb565_channel_error=maximum_error))
    return dict(edition=module, cases=results)


if __name__ == "__main__":
    result = dict(status="PASS", original_x86_cases=[cases(m) for m in ("classic", "game")],
                  original_half_size_cases=[half_constant_cases(m) for m in ('classic', 'game')],
                  note="Alpha is independent of RGB; two RGB565 channel units allow original lookup-table rounding.")
    path = ROOT / "generated/display-alpha-original.json"
    path.write_text(json.dumps(result, indent=2) + "\n")
    print("Original x86 UI alpha: 36 cases / two editions, full+half constant amount/pitch regression, opaque black/cyan/key, skip, per-pixel blend PASS")
