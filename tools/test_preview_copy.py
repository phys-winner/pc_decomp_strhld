"""Execute the SHA-pinned original 200x200 SAV preview copier, no device."""
import hashlib
import json
from pathlib import Path
import struct

from unicorn import UC_HOOK_CODE, UC_HOOK_MEM_WRITE
from unicorn.x86_const import *
from test_touch import Original, ROOT


def main():
    cases = []
    for edition in ("classic", "game"):
        classic = edition == "classic"
        fn, store = (0x4B5A40, 0x4B5A90) if classic else (0x4B5BD0, 0x4B5C20)
        context = 0x1A55350 if classic else 0x248C3A0
        screen = 0xFD5780 if classic else 0xFD5BE0
        original = Original(edition)
        u = original.u
        obj, ui, world = 0x30000000, 0x31000000, 0x33000000
        u.mem_map(obj, 0x80000)
        u.mem_map(ui, 0x1000000)
        u.mem_map(world, 0x1000000)
        colors = [0, 1, 0x07FF, 0xF81F, 0xFFFF] * 8000
        u.mem_write(obj + 0x4E244, struct.pack("<40000H", *colors))
        source_sha = hashlib.sha256(bytes(u.mem_read(obj + 0x4E244, 80000))).hexdigest()
        for mode in (0, 1):
            pitch = 3232 if mode == 0 else 8112
            target = ui if mode == 0 else world
            x, y = 55, 95
            original.w(screen + 0xD4, ui)
            original.w(screen + 0xD8, world)
            original.w(screen + 0x40, 3232)
            original.w(context + 12, mode)
            size = pitch * 300
            u.mem_write(target, b"\x34\x12" * (size // 2))
            guards = {UC_X86_REG_EBX: 0x11111111, UC_X86_REG_ESI: 0x22222222,
                      UC_X86_REG_EDI: 0x33333333, UC_X86_REG_EBP: 0x44444444}
            for reg, value in guards.items(): u.reg_write(reg, value)
            original.w(original.stack, original.end)
            original.w(original.stack + 4, x)
            original.w(original.stack + 8, y)
            u.reg_write(UC_X86_REG_ESP, original.stack)
            u.reg_write(UC_X86_REG_ECX, obj)
            u.reg_write(UC_X86_REG_EFLAGS, 2)
            writes = []
            def on_write(uc, access, address, width, value, user):
                if target <= address < target + size:
                    assert uc.reg_read(UC_X86_REG_EIP) == store
                    writes.append((address, width, value))
            def on_code(uc, pc, size, user):
                if pc == original.end: uc.emu_stop()
            tokens = [u.hook_add(UC_HOOK_MEM_WRITE, on_write), u.hook_add(UC_HOOK_CODE, on_code)]
            try: u.emu_start(fn, original.end + 1, count=1000000)
            finally:
                for token in tokens: u.hook_del(token)
            assert u.reg_read(UC_X86_REG_EIP) == original.end
            assert u.reg_read(UC_X86_REG_ESP) == original.stack + 12
            assert all(u.reg_read(reg) == value for reg, value in guards.items())
            expected = [(target + (y + row) * pitch + 2 * (x + col), 2, colors[row * 200 + col])
                        for row in range(200) for col in range(200) if colors[row * 200 + col]]
            assert writes == expected and len(writes) == 32000
            for row in range(200):
                got = struct.unpack("<200H", u.mem_read(target + (y + row) * pitch + 2 * x, 400))
                assert got == tuple(v or 0x1234 for v in colors[row * 200:(row + 1) * 200])
                assert original.rh(target + (y + row) * pitch + 2 * (x - 1)) == 0x1234
                assert original.rh(target + (y + row) * pitch + 2 * (x + 200)) == 0x1234
            assert original.r(context + 4) == target and original.r(context + 8) == pitch
            assert original.r(context + 12) == mode
            assert hashlib.sha256(bytes(u.mem_read(obj + 0x4E244, 80000))).hexdigest() == source_sha
            cases.append(dict(edition=edition, original_sha256=original.sha, function=hex(fn),
                              store_pc=hex(store), context=hex(context), mode=mode, pitch=pitch,
                              source_rgb565=[hex(v) for v in colors[:5]], pixels=40000,
                              zero_pixels_skipped=8000, nonzero_two_byte_stores=len(writes),
                              source_and_callee_saved_registers_unchanged=True,
                              context_base_pitch_and_stdcall8="PASS"))
    receipt = dict(status="PASS", scope="Unmodified original x86 preview copier and original context initializer; no leaf stubs, phone or native build.",
                   black_limit="RGB565 black is zero and the original skips it. Nonzero0x0001, cyan0x07FF, F81F and FFFF are copied exactly.", cases=cases)
    out = ROOT / "generated/preview-copy-original-proof.json"
    out.write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf8")
    print(f"Original Classic/Extreme SAV preview: PASS ({len(cases)} cases, zero skips/nonzero2-byte stores, original UI/world pitch selection)")


if __name__ == "__main__":
    main()
