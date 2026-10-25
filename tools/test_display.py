"""Derive viewport/edge regressions by executing the hash-pinned original x86.

Requires pefile and Unicorn on the build host; never starts either game.
The Android display.c self-check separately exercises the actual C helpers.
"""
import hashlib
import json
from pathlib import Path
import struct

import pefile
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE
from unicorn.x86_const import (UC_X86_REG_ECX, UC_X86_REG_ESP, UC_X86_REG_ESI, UC_X86_REG_EDI,
                              UC_X86_REG_EBX, UC_X86_REG_EBP, UC_X86_REG_EIP, UC_X86_REG_EAX)

from build_android import DISPLAY_LAYOUT, custom_display_dimensions, _display_hooks_seen

ROOT = Path(__file__).resolve().parents[1]
PRESETS = {1: (800, 600), 2: (1024, 768), 3: (1280, 720), 4: (1280, 1024),
           5: (1366, 768), 6: (1440, 900), 7: (1600, 900), 8: (1600, 1200),
           9: (1680, 1050), 10: (1920, 1080), 11: (1920, 1200), 12: (2560, 1440),
           13: (2560, 1600), 14: (1360, 768), 15: (1024, 600), 20: (640, 480)}
EDITIONS = {
    "classic": ("Stronghold Crusader.exe", 0x4E7090, 0xFD57DC, 0x21259AC,
                0x21288D8, 0xFD5854, 0xFD57C0, 0x43113D, 0x43117D),
    "game": ("Stronghold_Crusader_Extreme.exe", 0x4E7440, 0xFD5C3C, 0x2BB8E8C,
             0x2BBBDB8, 0xFD5CB4, 0xFD5C20, 0x4311BD, 0x4311FD),
}


def decode_tgx(data):
    width, height = struct.unpack_from("<II", data)
    rows = [[-1] * width for _ in range(height)]
    x = y = 0
    at = 8
    while at < len(data) and y < height:
        opcode = data[at]
        at += 1
        count, kind = (opcode & 31) + 1, opcode >> 5
        if kind == 4:
            y += 1
            x = 0
            continue
        assert x + count <= width
        if kind == 0:
            rows[y][x:x + count] = struct.unpack_from("<" + "H" * count, data, at)
            at += count * 2
        elif kind == 2:
            color = struct.unpack_from("<H", data, at)[0]
            at += 2
            rows[y][x:x + count] = [color] * count
        else:
            assert kind == 1, (at, opcode)
        x += count
    assert y == height and all(op == 0x80 for op in data[at:])
    return rows


class Original:
    def __init__(self, module):
        self.module = module
        (name, self.viewport, self.mode, self.game_mode, self.renderer, self.pixels,
         self.pitch, self.hud_start, self.hud_end) = EDITIONS[module]
        raw = (ROOT / "generated/input" / name).read_bytes()
        inputs = json.loads((ROOT / "config/inputs.json").read_text())
        self.image_name, self.sha256 = name, hashlib.sha256(raw).hexdigest()
        assert self.sha256 == inputs[name]
        self.powers_proof = []
        self.pe = pefile.PE(data=raw)
        image = self.pe.get_memory_mapped_image()
        self.u = Uc(UC_ARCH_X86, UC_MODE_32)
        self.u.mem_map(0x400000, (len(image) + 4095) & ~4095)
        self.u.mem_write(0x400000, image)
        self.obj, self.stack, self.dst, self.tgx = 0x10000000, 0x11000000, 0x12001000, 0x14000000
        self.u.mem_map(self.obj, 0x200000)
        self.u.mem_map(self.stack, 0x10000)
        self.u.mem_map(self.dst - 4096, 0x900000)
        self.u.mem_map(self.tgx, 0x200000)
        self.sp = self.stack + 0x8000

    def w(self, address, value):
        self.u.mem_write(address, struct.pack("<I", value & 0xFFFFFFFF))

    def r(self, address):
        return struct.unpack("<I", self.u.mem_read(address, 4))[0]

    def statistics_regression(self):
        ex = self.module == "game"
        initializers = ((0x4D6349, 0x4D6374), (0x4E28A3, 0x4E28EB)) if ex else (
                        (0x4D5F99, 0x4D5FC4), (0x4E24F3, 0x4E253B))
        saved_mode = 0xEFE0D4 if ex else 0xEFDC74
        cases = 0
        for mode, (width, height) in PRESETS.items():
            if height < 600:
                continue
            self.w(self.mode, mode)
            for start, stop in initializers:
                # Model the emitted operand for this CMP only; run the original
                # victory/defeat branches and their layout stores unchanged.
                def choose(uc, address, size, user):
                    self.w(self.mode, 1 if height < 768 else mode)
                def restore(uc, address, size, user):
                    self.w(self.mode, mode)
                hook = self.u.hook_add(UC_HOOK_CODE, choose, begin=start, end=start)
                after = self.u.hook_add(UC_HOOK_CODE, restore, begin=start+7, end=start+7)
                self.u.reg_write(UC_X86_REG_EBX, 0)
                self.u.emu_start(start, stop, count=30)
                expected = 0 if mode == 1 or height < 768 else 2
                assert self.r(self.game_mode + 4) == self.r(saved_mode) == expected
                self.u.hook_del(hook);self.u.hook_del(after)
                assert self.r(self.mode) == mode
                # Compact pages put their close control at y483; large pages at y543.
                assert (height - 600) // 2 + (483 if expected == 0 else 543) < height
                cases += 1
        for source in (ROOT / f"generated/{self.module}-x86").glob("*.c"):
            raw = source.read_bytes()
            if any(b"/* 0x%08X:" % pc in raw for pc in DISPLAY_LAYOUT[self.module]["statistics"]):
                patched, _ = custom_display_dimensions(raw, self.module)
                assert (b"MEM32(0x%Xu) < 768 ? 1u : " % (0xFD5C1C if ex else 0xFD57BC)) in patched
        assert all(f"{self.module}:{pc:08X}" in _display_hooks_seen for pc in DISPLAY_LAYOUT[self.module]["statistics"])
        return cases

    def viewport_regression(self):
        layout = DISPLAY_LAYOUT[self.module]
        stop = layout["camera"]
        hook = self.u.hook_add(UC_HOOK_CODE, lambda uc, a, size, user: uc.emu_stop(), begin=stop, end=stop)
        cases = []
        for mode, (width, height) in PRESETS.items():
            for zoom in (0, 1):
                for full in (0, 1):
                    viewheight = height if full else height - 128
                    self.u.mem_write(self.sp, struct.pack("<5I", 0, 0, 0, width, viewheight))
                    self.w(self.mode, mode)
                    self.w(self.game_mode, 12 if full else 0)
                    self.w(self.game_mode + 4, 60 if full else 0)
                    self.w(self.obj + 0x90, zoom)
                    self.u.reg_write(UC_X86_REG_ECX, self.obj)
                    self.u.reg_write(UC_X86_REG_ESP, self.sp)
                    self.u.emu_start(self.viewport, stop, count=1000)
                    scale = 2 if zoom else 1
                    assert self.r(self.obj + 0x88) == ((width + 31) // 32 + 5) * scale
                    assert self.r(self.obj + 0x8C) == (viewheight // 8 + 4) * scale
                    cases.append(dict(mode=mode, width=width, height=height, zoom=zoom, full=full,
                                      values={hex(off): self.r(self.obj + off) for off in
                                              (0x88, 0x8C, 0xAC, 0xB0, 0xA4, 0xA8, 0x18B730, 0x18B734)}))
        self.u.hook_del(hook)
        return cases

    def hud_regression(self, gfx):
        layout = DISPLAY_LAYOUT[self.module]
        calls = dict(layout["edges"][:2])
        original_calls = {a: bytes(self.u.mem_read(a, 5)) for a in calls}
        tests = 0
        for military in (False, True):
            prefix = "edge_military_" if military else "edge"
            for width, height in ((1280, 720), (1280, 600), (1304, 600), (1560, 720), (800, 600), (2560, 1440)):
                asset_width = 1280 if width == 1280 else 2560
                assets = [(gfx / f"{prefix}{asset_width}{side}.tgx").read_bytes() for side in "lr"]
                decoded = [decode_tgx(data) for data in assets]
                offset = 0
                for index, data in enumerate(assets):
                    iw, ih = struct.unpack_from("<II", data)
                    self.u.mem_write(self.tgx + offset, data)
                    descriptor = self.renderer + 0x16C20C + index * 16
                    self.w(descriptor, iw)
                    self.w(descriptor + 4, ih)
                    self.w(descriptor + 8, offset)
                    offset += len(data)
                self.w(self.renderer + 0x90, self.tgx)
                self.w(self.renderer + 8, 0)
                self.w(self.renderer + 0x54, 0)
                clip_before = (100, 50, width - 100, height - 250)
                for off, value in ((0x16C854, clip_before[0]), (0x16C858, clip_before[1]),
                                   (0x16C85C, clip_before[2]), (0x16C860, clip_before[3]),
                                   (0x60, 0), (0x64, height)):
                    self.w(self.renderer + off, value)
                self.w(self.pixels, self.dst)
                self.w(self.pitch, width * 2)
                size = width * height * 2
                self.u.mem_write(self.dst - 4096, b"\xA5" * (size + 8192))
                expected = bytearray(b"\xA5" * size)
                placements = []

                def edge_call(uc, address, length, user):
                    sp = uc.reg_read(UC_X86_REG_ESP)
                    index = self.r(sp)
                    bitmap = decoded[index]
                    iw, ih = len(bitmap[0]), len(bitmap)
                    x = (width - 800) // 2 - iw if not calls[address] else 1040 + (width - 1280) // 2
                    y = height - ih
                    self.w(sp + 4, x)
                    self.w(sp + 8, y)
                    if width != 1280:
                        for i, value in enumerate((0, 0, width, height)):
                            self.w(self.renderer + 0x16C854 + i * 4, value)
                    placements.append((index, x, y))
                    for by, row in enumerate(bitmap):
                        yy = y + by
                        for bx, color in enumerate(row):
                            xx = x + bx
                            if color >= 0 and 0 <= xx < width and 0 <= yy < height:
                                struct.pack_into("<H", expected, (yy * width + xx) * 2, color)

                def edge_return(uc, address, length, user):
                    if width != 1280:
                        for i, value in enumerate(clip_before):
                            self.w(self.renderer + 0x16C854 + i * 4, value)

                hooks = []
                for address in calls:
                    if width != 1280:
                        self.u.mem_write(address, b"\xE8" + struct.pack("<i", layout["clipped"] - address - 5))
                    else:
                        self.u.mem_write(address, original_calls[address])
                    self.u.ctl_remove_cache(address, address + 5)
                    hooks.append(self.u.hook_add(UC_HOOK_CODE, edge_call, begin=address, end=address))
                    hooks.append(self.u.hook_add(UC_HOOK_CODE, edge_return, begin=address + 5, end=address + 5))
                self.u.reg_write(UC_X86_REG_ESP, self.sp)
                self.u.reg_write(UC_X86_REG_ESI, 0)
                self.u.reg_write(UC_X86_REG_EDI, 1)
                self.u.emu_start(self.hud_start, self.hud_end, count=4000000)
                assert len(placements) == 2
                assert tuple(self.r(self.renderer + 0x16C854 + i * 4) for i in range(4)) == clip_before
                assert bytes(self.u.mem_read(self.dst - 4096, 4096)) == b"\xA5" * 4096
                guard = bytes(self.u.mem_read(self.dst + size, 4096))
                assert guard == b"\xA5" * 4096, (self.module, military, width, height, placements,
                                                 [(i, b) for i, b in enumerate(guard) if b != 0xA5][:8])
                actual = bytes(self.u.mem_read(self.dst, size))
                assert actual == expected, (self.module, military, width, height,
                                            sum(a != b for a, b in zip(actual, expected)))
                for hook in hooks:
                    self.u.hook_del(hook)
                if width == 1280 and height == 720:
                    assert placements == [(0, 0, 720 - len(decoded[0])), (1, 1040, 720 - len(decoded[1]))]
                tests += 1
        return tests

    def placement_regression(self):
        # Four HUD setup variants and both branches of the HUD refresh path.
        blocks = [(0x43113D, 0x43117D, 0, 0x80), (0x43205A, 0x43209A, 0, 0x210),
                  (0x4327C5, 0x432805, 0, 0x210), (0x432DAE, 0x432DEE, 0, 0x210),
                  (0x43A04C, 0x43A0B8, 1, 0x210), (0x43A04C, 0x43A0B8, 2, 0x210)]
        calls = dict(DISPLAY_LAYOUT[self.module]["edges"])
        tests = 0
        for start, end, special, shift in blocks:
            if self.module == "game":
                start += shift
                end += shift
            for width, height in ((1280, 720), (1304, 600), (1560, 720)):
                bitmap_width = 240 if width == 1280 else 880
                heights = [200, 128] if special else [128, 200]
                for index, bitmap_height in enumerate(heights):
                    self.w(self.renderer + 0x16C20C + index * 16, bitmap_width)
                    self.w(self.renderer + 0x16C210 + index * 16, bitmap_height)
                self.w(self.game_mode + 0x5C, 0 if special == 2 else 1)
                self.w(self.game_mode + 4, 0)
                positions = []

                def draw(uc, address, length, user):
                    sp = uc.reg_read(UC_X86_REG_ESP)
                    index, x, y = self.r(sp), self.r(sp + 4), self.r(sp + 8)
                    right = calls[address]
                    original_x = (1024 if special == 2 else 1040) if right else 0
                    assert (x, y) == (original_x, 720 - heights[index])
                    custom_x = x + (width - 1280) // 2 if right else (width - 800) // 2 - bitmap_width
                    custom_y = height - heights[index]
                    if width == 1280 and height == 720:
                        assert (custom_x, custom_y) == (x, y)
                    assert custom_y + heights[index] == height
                    if right:
                        assert custom_x == (width + 800) // 2 - (16 if special == 2 else 0)
                    else:
                        assert custom_x + bitmap_width == (width - 800) // 2
                    positions.append((index, custom_x, custom_y))
                    uc.reg_write(UC_X86_REG_ESP, sp + 12)
                    uc.reg_write(UC_X86_REG_EIP, address + 5)

                hooks = [self.u.hook_add(UC_HOOK_CODE, draw, begin=a, end=a) for a in calls]
                for reg, value in ((UC_X86_REG_ESP, self.sp), (UC_X86_REG_ESI, 16 if special else 0),
                                   (UC_X86_REG_EDI, 1), (UC_X86_REG_EBX, 0), (UC_X86_REG_EBP, 0)):
                    self.u.reg_write(reg, value)
                self.u.emu_start(start, end, count=1000)
                assert len(positions) == 2
                for hook in hooks:
                    self.u.hook_del(hook)
                tests += 1
        return tests

    def powers_regression(self):
        if self.module != "game":
            return 0
        # The startup setter has a separate mode table from options restore.
        # Execute its original constructor, geometry reader and hit-test.
        obj, stop = 0x020D2B50, self.obj + 0x3000
        self.w(0xDF76FC, 0)
        self.u.reg_write(UC_X86_REG_ESP, self.sp)
        self.u.emu_start(0x59D110, 0x59D144, count=1000)
        descriptor = self.r(0xDF76FC)
        assert descriptor == 0xDF8560
        assert [self.r(descriptor + i) for i in (0, 12, 16, 20)] == [130, 55, 320, 0x1000]
        assert self.u.reg_read(UC_X86_REG_ESP) == self.sp
        self.w(0x59F1F4, stop + 16)
        self.u.mem_write(stop + 16, b"\x31\xC0\xC3")  # GetTickCount fixture
        self.u.mem_write(0x46BE30, b"\xB8\x01\x00\x00\x00\xC3")  # in-game predicate fixture
        self.u.ctl_remove_cache(0x46BE30, 0x46BE36)
        self.u.mem_write(0x4F6FF0, b"\xC2\x04\x00")  # child control-list iteration fixture
        self.u.ctl_remove_cache(0x4F6FF0, 0x4F6FF3)
        self.w(0x2DD7920, 0)
        self.w(0x2DD7924, 0)
        cases = 0
        for width, height in ((1280, 720), (1304, 600), (1560, 720)):
            self.u.mem_write(obj, bytes(0x80))
            self.w(obj, 1)
            self.w(obj + 0x2C, 0xFFFFFFFF)
            self.w(0xFD5BF8, width)
            self.w(0xFD5BFC, height)
            self.w(0x431E38, width - 55)  # exact immediate of the validated startup hook
            self.u.ctl_remove_cache(0x431E35, 0x431E4B)
            self.u.reg_write(UC_X86_REG_ESP, self.sp)
            self.u.emu_start(0x431E35, 0x431E4B, count=1000)
            assert self.u.reg_read(UC_X86_REG_ESP) == self.sp
            assert (self.r(obj + 8), self.r(obj + 12)) == (width - 55, 56)

            # Original geometry getter used by rendering/child placement.
            outputs = [self.obj + 0x2000 + i * 4 for i in range(4)]
            self.u.mem_write(self.sp, struct.pack("<5I", stop, *outputs))
            self.u.reg_write(UC_X86_REG_ESP, self.sp)
            self.u.reg_write(UC_X86_REG_ECX, obj)
            self.u.emu_start(0x4AAC90, stop, count=1000)
            assert [self.r(a) for a in outputs] == [width - 55, 56, 55, 320]
            assert self.u.reg_read(UC_X86_REG_ESP) == self.sp + 20

            # Cursor boundaries are the same stored object coordinates.
            boundaries = []
            for cursor_x, inside in ((width - 56, 0), (width - 55, 1),
                                     (width - 1, 1), (width, 0),
                                     (1245, int(width == 1280))):
                self.w(0xF6A268, cursor_x)
                self.w(0xF6A26C, 100)
                self.u.mem_write(self.sp, struct.pack("<I", stop))
                self.u.reg_write(UC_X86_REG_ESP, self.sp)
                self.u.reg_write(UC_X86_REG_ECX, obj)
                self.u.emu_start(0x4AAAA0, stop, count=1000)
                # The original powers descriptor has flag0x1000: +78 is its
                # hit result; +74 deliberately stays clear for this sidebar.
                assert self.r(obj + 0x78) == inside, (width, cursor_x, inside)
                assert self.r(obj + 0x74) == 0
                child = self.r(obj + 0x24)
                assert child == 0xB99580
                assert (self.r(child + 4), self.r(child + 8)) == (width - 55, 56)
                assert self.u.reg_read(UC_X86_REG_ESP) == self.sp + 4
                boundaries.append(dict(cursor_x=cursor_x, cursor_y=100, inside=bool(inside), passed=True))
            self.powers_proof.append(dict(width=width, height=height, rect=[width - 55, 56, 55, 320],
                                          original_constructor=True, original_getrect=True,
                                          stack_cleanup_passed=True, boundaries=boundaries, passed=True))
            cases += 1
        return cases


def main():
    game = Path(json.loads((ROOT / "port.json").read_text())["game_directory"])
    rows = []
    for module in EDITIONS:
        original = Original(module)
        cases = original.viewport_regression()
        placements = original.placement_regression()
        count = original.hud_regression(game / "gfx")
        powers = original.powers_regression()
        statistics = original.statistics_regression()
        if powers:
            proof = dict(image=original.image_name, input_sha256=original.sha256,
                         startup_hook="00431E37", object="020D2B50", constructor="004AA950",
                         descriptor_initializer="0059D110", descriptor="00DF8560", descriptor_flags="00001000",
                         geometry_getter="004AAC90", hit_test="004AAAA0", hit_result_field="object+78",
                         child_position_propagated=True,
                         fixture_stubs=["GetTickCount=0", "in-game predicate=true", "child control-list iteration"],
                         cases=original.powers_proof)
            (ROOT / "generated/powers-original-proof.json").write_text(json.dumps(proof, indent=2) + "\n")
        rows.extend(dict(edition=module, **case) for case in cases)
        print(f"{module}: {len(cases)} original viewport states; {placements} HUD branch placements; "
              f"{count} original TGX HUD draws/canaries; {powers} powers geometry/hit-test states; "
              f"{statistics} statistics layouts/close controls: PASS", flush=True)
    (ROOT / "generated/viewport-original-presets.json").write_text(json.dumps(rows, indent=2) + "\n")


if __name__ == "__main__":
    main()
