"""Execute real, SHA-pinned Stronghold x86 touch decision leaves in Unicorn.

The original code selects units and writes its own order payloads. The emulator
only stops at the existing command queue / external leaf call, never starts the
Windows game and never modifies either EXE.
"""
import hashlib
import json
from pathlib import Path
import struct

import capstone
import pefile
from unicorn import Uc, UcError, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE, UC_HOOK_MEM_INVALID
from unicorn.x86_const import *
from touch_hooks import LAYOUT, apply as apply_touch_hooks

ROOT = Path(__file__).resolve().parents[1]
EDITIONS = {
    "game": dict(name="Stronghold_Crusader_Extreme.exe", units=0x1593698,
                 player=0x25F874C, pick=0x53E7D0, same=0x535EC0, clear=0x535F60,
                 anchor=0x4687D0, queue=0x489BF0, payload=0x2506C38,
                 move=0x537C40, target=0x537F70, group=0x20D2BD0,
                 context=0x2BB8D50, subtype=0x15936A8, waypoints=0x20D2BEC,
                 targetid=0x2BB8D60, unitgen=0x1593D44),
    "classic": dict(name="Stronghold Crusader.exe", units=0x14BEBC8,
                    player=0x1B6526C, pick=0x53E380, same=0x535A70, clear=0x535B10,
                    anchor=0x4685D0, queue=0x489AC0, payload=0x1ACFBE8,
                    move=0x5377F0, target=0x537B20, group=0x179FBA8,
                    context=0x2125870, subtype=0x14BEBD8, waypoints=0x179FBC4,
                    targetid=0x2125880, unitgen=0x14BF274),
}


class Original:
    def __init__(self, edition):
        self.edition, self.d, self.layout = edition, EDITIONS[edition], LAYOUT[edition]
        raw = (ROOT / "generated/input" / self.d["name"]).read_bytes()
        self.sha = hashlib.sha256(raw).hexdigest()
        assert self.sha == json.loads((ROOT / "config/inputs.json").read_text())[self.d["name"]]
        self.pe = pefile.PE(data=raw)
        image = self.pe.get_memory_mapped_image()
        self.u = Uc(UC_ARCH_X86, UC_MODE_32)
        self.u.mem_map(0x400000, (len(image) + 4095) & ~4095)
        self.u.mem_write(0x400000, image)
        self.stack, self.end, self.scratch = 0x10008000, 0x20000000, 0x10010000
        self.u.mem_map(0x10000000, 0x20000)
        self.u.mem_map(self.end, 4096)
        self.w(self.d["payload"] + 12, self.scratch)
        self.w(self.d["player"], 1)
        self.events = []

    def w(self, a, v): self.u.mem_write(a, struct.pack("<I", v & 0xFFFFFFFF))
    def h(self, a, v): self.u.mem_write(a, struct.pack("<H", v & 65535))
    def r(self, a): return struct.unpack("<I", self.u.mem_read(a, 4))[0]
    def rh(self, a): return struct.unpack("<H", self.u.mem_read(a, 2))[0]

    def run(self, address, args=(), this=0, hook=None, frame=False):
        self.u.mem_write(self.stack - 256, bytes(1024))
        ret = self.stack + 52 if frame else self.stack
        self.w(ret, self.end)
        for i, a in enumerate(args): self.w(ret + 4 + i * 4, a)
        self.u.reg_write(UC_X86_REG_ESP, self.stack)
        self.u.reg_write(UC_X86_REG_ECX, this)
        self.u.reg_write(UC_X86_REG_EFLAGS, 2)
        def on_code(uc, pc, size, user):
            if pc == self.end: uc.emu_stop()
            elif hook: hook(pc)
        token = self.u.hook_add(UC_HOOK_CODE, on_code)
        try: self.u.emu_start(address, self.end + 1, count=100000)
        finally: self.u.hook_del(token)
        return self.u.reg_read(UC_X86_REG_EAX)

    def box_receiver(self):
        classic = self.edition == "classic"
        renderer = 0x21288D8 if classic else 0x2BBBDB8
        drawing = 0x1A55350 if classic else 0x248C3A0
        draw = 0x471750 if classic else 0x471960
        screen = 0xFD5780 if classic else 0xFD5BE0
        source_call = 0x47331B if classic else 0x47353B
        # The original selection outline supplies this exact primitive object.
        assert self.pe.get_data(source_call - 15 - 0x400000, 5) == b"\xB9" + struct.pack("<I", drawing)
        world = 0x30000000
        self.u.mem_map(world, 0x1100000)
        self.w(screen + 0xD4, world); self.w(screen + 0xD8, world)
        self.w(screen + 0x40, 3120)
        cases = []
        for wrong in (True, False):
            self.u.mem_write(renderer, bytes(256))
            self.w(renderer + 8, 0); self.w(renderer + 12, world)
            self.w(renderer + 0x58, 0); self.w(renderer + 0x5C, 2076)
            self.w(renderer + 0x60, 0); self.w(renderer + 0x64, 720)
            # The original horizontal/vertical leaves address the shared
            # primitive object's base/stride even when their receiver is wrong.
            self.w(drawing + 4, world); self.w(drawing + 8, 8112)
            self.w(drawing + 12, 1)
            before = bytes(self.u.mem_read(renderer, 256))
            receiver = renderer if wrong else drawing
            self.run(draw, (110, 120, 210, 220, 0x7BEF), receiver)
            assert self.u.reg_read(UC_X86_REG_EIP) == self.end
            assert self.u.reg_read(UC_X86_REG_ESP) == self.stack + 24
            if wrong:
                assert self.r(renderer + 4) == world
                assert self.r(renderer + 8) == 8112
            else:
                assert bytes(self.u.mem_read(renderer, 256)) == before
                assert self.r(drawing + 8) == 8112
            cases.append(dict(receiver=hex(receiver), original_function=hex(draw),
                              wrong_receiver=wrong, tgx_mode=self.r(renderer + 8),
                              tgx_256bytes_unchanged=bytes(self.u.mem_read(renderer, 256)) == before,
                              stdcall_stack="PASS"))
        return dict(original_selection_call=hex(source_call), expected_receiver=hex(drawing),
                    wrong_receiver_reproduces_pitch8112_corruption=True,
                    corrected_receiver_preserves_tgx=True, cases=cases)

    def ui_pixel_mask(self):
        classic = self.edition == "classic"
        fn, obj = (0x4632C0, 0xF69C04) if classic else (0x4634F0, 0xF6A064)
        source = ROOT / f"generated/{self.edition}-x86" / ("recomp_0021.c" if classic else "recomp_0020.c")
        patched, _ = apply_touch_hooks(source.read_bytes(), self.edition)
        body = patched.split(f"void sub_{fn:08X}(void) {{".encode(), 1)[1].split(b"\nvoid ", 1)[0]
        guard = (f"if ((int32_t)stronghold_touch_ui_x(MEM32(0x{self.layout['input']+16:X}u)) < 0 || "
                 f"(int32_t)stronghold_touch_ui_y(MEM32(0x{self.layout['input']+20:X}u)) < 0) {{ "
                 "eax=0; esp+=4; RECOMP_FLAGS_OUT(); RECOMP_REGS_OUT(); return; }").encode()
        assert guard in body and body.index(guard) < body.index(b" /* 0x"), "mask entry guard must run before original code"
        wrapped_reads = {}
        for axis, offset in (("x", 16), ("y", 20)):
            token = f"MEM32(0x{self.layout['input']+offset:X})".encode()
            wrapped = f"stronghold_touch_ui_{axis}(".encode() + token + b")"
            assert body.count(token) and body.count(token) == body.count(wrapped), (self.edition, axis, "raw mask coordinate read remains")
            wrapped_reads[axis] = body.count(wrapped)
        fill, paint = (0x4724C0, 0x455500) if classic else (0x4726D0, 0x455740)
        state = 0xF10604 if classic else 0xF10A64
        group, frames, descriptor = ((0xB353E0, 0xD7EED8, 0xB9A7D8) if classic
                                     else (0xB35570, 0xD7F058, 0xB9A958))
        origin, key = (0xF105F0, 0xDF53E8) if classic else (0xF10A50, 0xDF5568)
        target = 0x33001000
        self.u.mem_map(target, 8192)  # Guard page before the64x64 mask is unmapped.
        self.w(obj, target); self.w(group, 0); self.w(group + 12, 0); self.w(frames, 0)
        self.h(descriptor, 64); self.h(descriptor + 2, 64); self.h(key, 0xF81F)
        self.w(origin, 0); self.w(origin + 4, 0); self.w(state, 0)
        result = []
        for frame in (1, -1):
            for point in ((-1, -1), (-1, 5), (5, -1), (5, 5)):
                for guarded in (False, True):
                    self.w(state + 4, frame)
                    self.w(self.layout["input"] + 16, point[0])
                    self.w(self.layout["input"] + 20, point[1])
                    self.u.mem_write(target, b"\x1F\x00" * 4096)
                    faults, calls = [], []
                    def invalid(uc, access, address, size, value, user):
                        faults.append(dict(pc=hex(uc.reg_read(UC_X86_REG_EIP)), address=hex(address)))
                        return False
                    def hook(pc):
                        sp = self.u.reg_read(UC_X86_REG_ESP)
                        if pc == fn and guarded and min(point) < 0:
                            # Entry contract of the existing ui_x/ui_y guard:
                            # no active native UI point cannot enter the mask reader.
                            self.u.reg_write(UC_X86_REG_EAX, 0)
                            self.u.reg_write(UC_X86_REG_EIP, self.r(sp))
                            self.u.reg_write(UC_X86_REG_ESP, sp + 4)
                        elif pc in (fill, paint):
                            calls.append(hex(pc))
                            self.u.reg_write(UC_X86_REG_EIP, self.r(sp))
                            self.u.reg_write(UC_X86_REG_ESP, sp + (16 if pc == fill else 20))
                    token = self.u.hook_add(UC_HOOK_MEM_INVALID, invalid)
                    try:
                        try: value = self.run(fn, this=obj, hook=hook)
                        except UcError:
                            assert faults and not guarded and point[1] < 0
                            value = None
                    finally: self.u.hook_del(token)
                    if guarded and min(point) < 0:
                        assert value == 0 and not calls and not faults
                    elif min(point) >= 0:
                        assert value == 1 and not faults and len(calls) == 2
                    elif point[1] < 0:
                        assert faults
                    else:
                        # NegativeX can address the previous row without a hardware
                        # fault: the mask still reports a false geometric hit.
                        assert value == 1
                    result.append(dict(frame=frame, point=point, entry_guard=guarded,
                                       result=value, faults=faults, original_helper_calls=calls))
        return dict(function=hex(fn), scope="Original mask reader executed; fill/sprite paint leaves stubbed. Entry guard contract simulated at the original function entry.",
                    invalid_y_fault_reproduced=True, negative_x_false_hit_reproduced=True,
                    guarded_inactive_returns_false=True, active_ui_point_preserved=True,
                    generated_entry_guard_verified=True, generated_body_wrapped_reads=wrapped_reads, cases=result)

    def unit_fixture(self):
        base = self.d["units"]
        self.w(base, 7)
        for ident, owner, kind, x, dead in ((1,1,22,310,0), (2,1,22,360,0),
                                          (3,1,41,410,0), (4,2,22,460,0),
                                          (5,1,22,510,1), (6,1,22,0,0)):
            at = base + 1168 * ident
            for off, value in ((1696,2),(1698,kind),(1706,owner),(2232,1),
                               (2228,dead),(1576,x),(1578,210),(1586,24),(1588,30)):
                self.h(at + off, value)

    def selection(self):
        self.unit_fixture()
        inp, mapobj = self.layout["input"], self.layout["map"]
        tests = []
        for zoom in (0,1):
            self.w(mapobj + 0x80, 96); self.w(mapobj + 0x84, 48); self.w(mapobj + 0x90, zoom)
            self.w(inp + 0x10, 214); self.w(inp + 0x14, 162)
            self.run(self.layout["rect"], (4,4), inp)
            actual = [self.r(inp + o) for o in (0x80,0x84,0x88,0x8C)]
            scale = 2 if zoom else 1
            assert actual == [308*scale,208*scale,312*scale,212*scale], actual
            tests.append(dict(zoom=zoom, rectangle=actual))
        self.w(mapobj + 0x90, 0)
        self.run(self.layout["rect"], (4,4), inp)
        assert self.run(self.d["pick"], (5,), self.d["units"]) == 1
        # Same type is scoped by original liveness, owner and visible-screen
        # state: enemy, dead, other type and offscreen units are excluded.
        self.run(self.d["clear"], this=self.d["units"])
        self.run(self.d["same"], (1,22), self.d["units"])
        selected = [i for i in range(1,7) if self.rh(self.d["units"]+1168*i+1608)]
        assert selected == [1,2], selected
        assert self.r(self.d["units"] + 32) == 2
        assert self.u.reg_read(UC_X86_REG_ESP) == self.stack + 12
        return dict(pick=1, same_type=selected, rectangle_cases=tests, stdcall_stack="PASS")

    def payload(self, function, args, expected_command, expected_payload):
        records = []
        def capture(pc):
            if pc == self.d["queue"]:
                sp = self.u.reg_read(UC_X86_REG_ESP)
                values = [self.r(self.d["payload"] + o) for o in (0,4,8,12,16)]
                records.append((self.r(sp+4), values))
                self.u.emu_stop()
        self.run(function, args, self.d["units"], capture)
        assert records == [(expected_command, expected_payload)], records
        return dict(command=expected_command, payload=expected_payload)

    def lord_selection(self):
        ex=self.edition=="game";units=self.d["units"]
        self.unit_fixture()
        self.w(units,2);u=units+1168
        for offset,value in ((1696,2),(1698,55),(1706,1),(1608,1),(1734,1),(2228,0)):
            self.h(u+offset,value)
        self.w(0x248CA00 if ex else 0x1A559B0,0)
        for offset in (0x60,0x70,0x80):
            for index,value in enumerate((308,208,312,212)):
                self.w(self.layout["input"]+offset+index*4,value)
        assert self.run(self.d["pick"],(5,),units)==1
        self.h(u+1608,0)
        self.run(0x5355F0 if ex else 0x5351A0,this=units)
        assert self.rh(u+1608)==1 and self.r(units+32)==1
        queued=[]
        def ret(n,result=0):
            sp=self.u.reg_read(UC_X86_REG_ESP);self.u.reg_write(UC_X86_REG_EAX,result)
            self.u.reg_write(UC_X86_REG_ESP,sp+4+4*n);self.u.reg_write(UC_X86_REG_EIP,self.r(sp))
        def stub(pc):
            if pc==(0x472700 if ex else 0x4724F0):ret(3)
            elif pc==(0x523530 if ex else 0x5230E0):ret(1,7)
            elif pc==self.d["queue"]:
                sp=self.u.reg_read(UC_X86_REG_ESP);queued.append(self.r(sp+4));ret(1)
        filtered=0x536840 if ex else 0x5363F0
        click=0x536910 if ex else 0x5364C0
        assert self.run(filtered,this=units,hook=stub)==0 and not queued
        assert self.run(click,this=units,hook=stub)==1 and queued==[16]
        assert self.u.reg_read(UC_X86_REG_ESP)==self.stack+4
        source=ROOT/f"generated/{self.edition}-x86/recomp_0010.c"
        patched,_=apply_touch_hooks(source.read_bytes(),self.edition)
        assert b"if (stronghold_touch_group_unfiltered())" in patched
        gate=self.layout["lord_lock"];allowed=self.layout["lord_allowed"]
        assert (b"if (stronghold_touch_lord_control()) goto L_%08X;" % allowed) in patched
        permission=[]
        for native in (False,True):
            for operation,context in ((0,2),(10,2),(-1,4)):
                self.u.reg_write(UC_X86_REG_EDI,0)
                def permit(pc):
                    if pc==gate:
                        sp=self.u.reg_read(UC_X86_REG_ESP)
                        self.w(sp+0x18,0);self.w(sp+0x14,context)
                        self.w(self.d["context"],operation)
                        if native:self.u.reg_write(UC_X86_REG_EIP,allowed)
                    elif pc==self.layout["order"]:self.u.emu_stop()
                self.run(gate,hook=permit,frame=True)
                actual=self.r(self.d["context"])
                actual=actual if actual<0x80000000 else actual-0x100000000
                result=(self.u.reg_read(UC_X86_REG_ESI),actual)
                assert result==((context,operation) if native else (4,-1)),result
                permission.append(dict(native=native,evaluated_context=context,evaluated_operation=operation,
                                       retained_context=result[0],retained_operation=result[1]))
        self.unit_fixture();self.h(u+1734,0)
        return dict(lord_picker=1,lord_rectangle_selected=True,lord_singleplayer_filtered=0,desktop_click_group=1,original_command=16,
                    native_uses_desktop_lord_selection=True,native_lord_permission=permission,
                    invalid_destination_stays_invalid=True)

    def owned_fortification(self):
        gate=self.layout["fortification_gate"];allowed=self.layout["fortification_allowed"]
        mapobj=self.layout["map"];flags=self.layout["tile_flags"];owner=self.layout["tile_owner"]
        md=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_32);md.detail=True
        instructions=list(md.disasm(bytes(self.u.mem_read(gate,16)),gate))
        assert instructions[0].mnemonic=="cmp" and instructions[0].operands[0].mem.disp==mapobj+108
        assert instructions[0].operands[1].imm==1
        assert instructions[1].mnemonic in ("jne","jnz") and instructions[1].operands[0].imm==allowed
        accepted=next(md.disasm(bytes(self.u.mem_read(allowed,8)),allowed))
        assert accepted.mnemonic=="mov" and accepted.op_str=="esi, 2"
        delta=0x210 if self.edition=="game" else 0
        wall_test=next(md.disasm(bytes(self.u.mem_read(0x43640F+delta,12)),0x43640F+delta))
        owner_test=list(md.disasm(bytes(self.u.mem_read(0x436489+delta,24)),0x436489+delta))
        assert wall_test.mnemonic=="test" and wall_test.operands[0].mem.disp==flags
        assert wall_test.operands[1].imm==0x10000300, "Original fortification classes include keep platforms"
        assert owner_test[0].mnemonic=="movzx" and owner_test[0].operands[1].mem.disp==owner
        assert owner_test[3].mnemonic=="cmp" and owner_test[3].operands[1].mem.disp==self.d["player"]
        assert self.layout["player"]==self.d["player"]
        last_click=next(md.disasm(bytes(self.u.mem_read(0x437572+delta,8)),0x437572+delta))
        assert last_click.mnemonic=="mov"
        last_click=last_click.operands[0].mem.disp
        source=ROOT/f"generated/{self.edition}-x86/recomp_0010.c"
        patched,_=apply_touch_hooks(source.read_bytes(),self.edition)
        begin=patched.index(b"L_%08X:"%gate);end=patched.index(b"/* cmp",begin)
        emitted=patched[begin:end]
        for expression in (
            b"uint32_t touch_tile = MEM32(0x%Xu)"%(mapobj+96),
            b"stronghold_touch_lord_control() && touch_tile &&",
            b"(!MEM32(0x%Xu) || touch_tile == MEM32(0x%Xu)) && touch_tile == MEM32(0x%Xu)"%(mapobj+84,mapobj+84,mapobj+112),
            b"(MEM32(0x%Xu + touch_tile * 4u) & 0x10000300u)"%flags,
            b"((MEM8(0x%Xu + touch_tile) & 7u) + 1u) == MEM32(0x%Xu)"%(owner,self.d["player"]),
            b"goto L_%08X;"%allowed,
        ): assert expression in emitted,expression
        self.unit_fixture();units=self.d["units"];u=units+1168;tile=3
        self.w(units,2);self.w(units+32,1);self.w(self.d["group"],7)
        self.w(self.d["subtype"],1);self.w(self.d["waypoints"],0)
        for offset,value in ((1696,2),(1706,1),(1608,1),(1734,1),(2228,0)):
            self.h(u+offset,value)
        self.w(0x248CA00 if self.edition=="game" else 0x1A559B0,0)
        self.w(0x59F1F4,self.end+16)
        cases=[]
        fixtures=(
            ("owned_wall",True,tile,tile,tile,0x100,0,True),
            ("owned_stairs",True,tile,tile,tile,0x900,0,True),
            ("owned_crenellation",True,tile,tile,tile,0x200,0,True),
            ("owned_keep_platform",True,tile,tile,tile,0x10000000,0,True),
            ("owned_keep_empty_surface84",True,0,tile,tile,0x10008000,0,True),
            ("owned_with_upper_owner_bits",True,tile,tile,tile,0x100,0xA8,True),
            ("desktop_unchanged",False,tile,tile,tile,0x100,0,False),
            ("different_visible_surface",True,tile+1,tile,tile,0x100,0,False),
            ("different_raw_surface",True,tile,tile,tile+1,0x100,0,False),
            ("foreign_wall",True,tile,tile,tile,0x100,1,False),
            ("foreign_keep_platform",True,tile,tile,tile,0x10000000,1,False),
            ("foreign_keep_empty_surface84",True,0,tile,tile,0x10008000,1,False),
            ("empty_surface84_different_raw",True,0,tile,tile+1,0x10008000,0,False),
            ("empty_surface84_ground",True,0,tile,tile,0x8000,0,False),
            ("ordinary_ground",True,tile,tile,tile,0x8000,0,False),
            ("ordinary_building",True,tile,tile,tile,0x80,0,False),
            ("empty_surface",True,0,0,0,0x100,0,False),
        )
        for kind in (22,55,70,72,73):
            self.h(u+1698,kind)
            for name,native,visual,target,raw,tileflags,tileowner,expected in fixtures:
                for offset,value in ((84,visual),(96,target),(100,80),(104,95),(108,1),(112,raw)):
                    self.w(mapobj+offset,value)
                self.w(flags+target*4,tileflags);self.u.mem_write(owner+target,bytes([tileowner]))
                self.w(self.d["context"],0)
                self.w(last_click,0)
                self.u.reg_write(UC_X86_REG_EDI,0);self.u.reg_write(UC_X86_REG_EBX,0)
                self.u.reg_write(UC_X86_REG_EBP,1)
                record=dict(case=name,unit_kind=kind,native_eligible=native)
                queued=[]
                def hook(pc):
                    sp=self.u.reg_read(UC_X86_REG_ESP)
                    if pc==gate:
                        # Simulate the emitted native predicate at its exact
                        # instruction; all following classification/dispatch
                        # and command payload stores are original x86.
                        t=self.r(mapobj+96)
                        own=(self.u.mem_read(owner+t,1)[0]&7)+1==self.r(self.d["player"])
                        if native and t and self.r(mapobj+84) in (0,t) and t==self.r(mapobj+112) and self.r(flags+4*t)&0x10000300 and own:
                            self.u.reg_write(UC_X86_REG_EIP,allowed)
                    elif pc==self.layout["lord_lock"] and native:
                        self.u.reg_write(UC_X86_REG_EIP,self.layout["lord_allowed"])
                    elif pc==self.layout["order"]:
                        context=self.u.reg_read(UC_X86_REG_ESI);op=self.r(self.d["context"])
                        record.update(context=context,operation=op if op<0x80000000 else op-0x100000000)
                        if expected:
                            assert (context,op)==(2,0),record
                            self.u.reg_write(UC_X86_REG_EBX,107);self.u.reg_write(UC_X86_REG_EBP,1)
                            self.u.reg_write(UC_X86_REG_EIP,self.layout["dispatch"])
                        else:
                            assert (context,op)==(4,0xFFFFFFFF),record
                            self.u.emu_stop()
                    elif pc==self.end+16 or pc==(0x468940 if self.edition=="game" else 0x468740):
                        self.u.reg_write(UC_X86_REG_EAX,1000 if pc==self.end+16 else 0)
                        self.u.reg_write(UC_X86_REG_ESP,sp+4);self.u.reg_write(UC_X86_REG_EIP,self.r(sp))
                    elif pc==self.d["queue"]:
                        queued.append((self.r(sp+4),[self.r(self.d["payload"]+i) for i in range(0,20,4)]))
                        self.u.emu_stop()
                self.run(gate,hook=hook,frame=True)
                assert "context" in record,record
                assert queued==([(17,[7,80,95,0,1])] if expected else []),(record,queued)
                assert self.r(mapobj+108)==1 and self.r(mapobj+96)==target
                assert self.r(flags+4*target)==tileflags and self.u.mem_read(owner+target,1)[0]==tileowner
                record.update(queued=queued,original_target_and_flags_unchanged=True)
                cases.append(record)
        return dict(scope="Original raised-target leaf and move dispatcher/queue payload executed starting at the gate. Preceding route/capability checks are exercised separately by fortification_eligibility. Exact native predicate and existing lord/tap seams simulated; only clock/feedback reset external leaves stubbed. No pathfinding bypass or tile mutation.",
                    original_pc_and_data_addresses_verified=True,emitted_guards_verified=True,cases=cases)

    def fortification_eligibility(self):
        # Start before the original route/capability checks, rather than at
        # the raised-surface leaf. In particular, a valid owned keep must not
        # turn siege-only, foreign, dead or disconnected selections into moves.
        ex=self.edition=="game";delta=0x210 if ex else 0
        entry=0x436B40+delta;units=self.d["units"];mapobj=self.layout["map"]
        flags=self.layout["tile_flags"];owner=self.layout["tile_owner"]
        gate=self.layout["fortification_gate"];allowed=self.layout["fortification_allowed"]
        cache=0x536050 if ex else 0x535C00
        representative=0x537870 if ex else 0x537420
        capability=0x537070 if ex else 0x536C20
        route=0x4A5BD0 if ex else 0x4A5A40
        special_route=0x4A63B0 if ex else 0x4A6220
        md=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_32);md.detail=True
        instructions=list(md.disasm(bytes(self.u.mem_read(entry,0x1EA)),entry))
        assert instructions[0].mnemonic=="mov" and instructions[0].operands[1].mem.disp==flags
        assert instructions[0].operands[1].mem.index==capstone.x86.X86_REG_EAX
        zone=instructions[1].operands[1].mem.disp
        calls={i.operands[0].imm for i in instructions if i.mnemonic=="call"}
        assert {representative,capability,route,special_route}<=calls
        # Decode the earlier original occupancy lookup, avoiding a second
        # hand-maintained edition-specific map-array address in the fixture.
        building=next(md.disasm(bytes(self.u.mem_read(0x436C99+delta,8)),0x436C99+delta)).operands[1].mem.disp
        last_click=next(md.disasm(bytes(self.u.mem_read(0x437572+delta,8)),0x437572+delta)).operands[0].mem.disp
        self.w(0x59F1F4,self.end+16)
        fixtures=[]
        for kind in (22,55,70,71,72,73,75,76):
            fixtures.append((f"owned_keep_kind_{kind}",[(kind,1,0,1)],1,1,0,True,True,True))
        for kind in (40,41,61,74,77):
            fixtures.append((f"unsupported_keep_kind_{kind}",[(kind,1,0,1)],1,1,0,True,False,False))
        for kind in (22,55,70,73):
            fixtures.append((f"missing_destination_zone_kind_{kind}",[(kind,1,0,1)],1,0,0,True,False,False))
        fixtures.extend((
            ("foreign_selection",[(22,2,0,1)],1,1,0,True,False,False),
            ("dead_selection",[(22,1,1,1)],1,1,0,True,False,False),
            ("empty_selection",[(22,1,0,0)],1,1,0,True,False,False),
            ("foreign_keep",[(22,1,0,1)],1,1,1,True,True,False),
            ("desktop_keeps_original_denial",[(22,1,0,1)],1,1,0,False,True,False),
            # A mixed selection follows the original representative-unit and
            # capability policy; the native seam does not reclassify its units.
            ("mixed_foot_and_siege",[(22,1,0,1),(40,1,0,1)],1,1,0,True,True,True),
            ("mixed_siege_and_foot",[(40,1,0,1),(22,1,0,1)],1,1,0,True,True,True),
        ))
        cases=[]
        for name,selected,source_zone,target_zone,target_owner,native,reaches_gate,accepted in fixtures:
            self.u.mem_write(units,bytes(1168*4+2500))
            self.w(units,len(selected)+1);self.w(units+32,sum(v[3] for v in selected))
            for ident,(kind,unit_owner,dead,is_selected) in enumerate(selected,1):
                u=units+1168*ident
                for offset,value in ((1696,2),(1698,kind),(1706,unit_owner),(1608,is_selected),(1734,1),(2228,dead)):
                    self.h(u+offset,value)
                self.w(u+1768,4)
            self.w(flags+4*4,0x8000);self.w(flags+4*3,0x10000000)
            self.h(zone+4*2,source_zone);self.h(zone+3*2,target_zone)
            self.h(building+3*2,0)
            for offset,value in ((84,0),(96,3),(100,80),(104,95),(108,1),(112,3)):
                self.w(mapobj+offset,value)
            self.u.mem_write(owner+3,bytes([target_owner]));self.w(self.d["context"],0)
            self.w(last_click,0)
            self.w(self.d["group"],7);self.w(self.d["subtype"],1);self.w(self.d["waypoints"],0)
            # Run the actual histogram rebuild. Kind73 takes a separate
            # original route check; leaving stale zero counters would miss it.
            self.run(cache,this=units)
            self.u.reg_write(UC_X86_REG_EAX,3);self.u.reg_write(UC_X86_REG_EBX,0)
            self.u.reg_write(UC_X86_REG_EDI,0)
            visited=[];queued=[];record=dict(case=name,selected=selected)
            def hook(pc):
                sp=self.u.reg_read(UC_X86_REG_ESP)
                if pc in (representative,capability,route,special_route):visited.append(pc)
                if pc==gate:
                    record["reached_native_gate"]=True
                    t=self.r(mapobj+96)
                    own=(self.u.mem_read(owner+t,1)[0]&7)+1==self.r(self.d["player"])
                    if native and t and self.r(mapobj+84) in (0,t) and t==self.r(mapobj+112) and self.r(flags+4*t)&0x10000300 and own:
                        self.u.reg_write(UC_X86_REG_EIP,allowed)
                elif pc==self.layout["lord_lock"] and native:
                    self.u.reg_write(UC_X86_REG_EIP,self.layout["lord_allowed"])
                elif pc==self.layout["order"]:
                    context=self.u.reg_read(UC_X86_REG_ESI);operation=self.r(self.d["context"])
                    record.update(context=context,operation=operation if operation<0x80000000 else operation-0x100000000)
                    if accepted:
                        assert (context,operation)==(2,0),record
                        self.u.reg_write(UC_X86_REG_EBX,107);self.u.reg_write(UC_X86_REG_EBP,1)
                        self.u.reg_write(UC_X86_REG_EIP,self.layout["dispatch"])
                    else:
                        assert context==4 and operation==(0xFFFFFFFF if reaches_gate else 0xFFFFFFF6),record
                        self.u.emu_stop()
                elif pc==self.end+16 or pc==(0x468940 if ex else 0x468740):
                    self.u.reg_write(UC_X86_REG_EAX,1000 if pc==self.end+16 else 0)
                    self.u.reg_write(UC_X86_REG_ESP,sp+4);self.u.reg_write(UC_X86_REG_EIP,self.r(sp))
                elif pc==self.d["queue"]:
                    queued.append((self.r(sp+4),[self.r(self.d["payload"]+i) for i in range(0,20,4)]))
                    self.u.emu_stop()
            self.run(entry,hook=hook,frame=True)
            assert "context" in record and record.get("reached_native_gate",False)==reaches_gate,record
            assert representative in visited and capability in visited,(record,visited)
            if any(v[0]==73 for v in selected):assert special_route in visited,(record,visited)
            assert queued==([(17,[7,80,95,0,1])] if accepted else []),(record,queued)
            assert self.r(mapobj+96)==3 and self.r(mapobj+108)==1
            assert self.r(flags+4*3)==0x10000000 and self.u.mem_read(owner+3,1)[0]==target_owner
            record.update(original_checks=[hex(pc) for pc in visited],queued=queued)
            cases.append(record)
        return dict(scope="Original representative selection, fresh class histogram, occupancy/capability checks, same/missing-zone routing, raised-target decision and final move queue executed. Native decision seams and clock/feedback leaves simulated only; no unit/route helper stubbed.",
                    earlier_original_target_flags_read_verified=True,cases=cases)

    def dispatch(self, operation, target, expected_args):
        self.w(self.d["subtype"],1); self.w(self.d["context"],operation)
        self.w(self.d["group"],7); self.w(self.d["waypoints"],0)
        self.w(self.layout["map"]+0x64,80); self.w(self.layout["map"]+0x68,95)
        self.w(self.d["targetid"],4); self.w(self.d["unitgen"]+292*4*4,1234)
        self.u.reg_write(UC_X86_REG_ESI,2); self.u.reg_write(UC_X86_REG_EDI,0)
        self.u.reg_write(UC_X86_REG_EBX,107); self.u.reg_write(UC_X86_REG_EBP,1)
        # timeGetTime thunk only needed by the normal move feedback branch.
        self.w(0x59F1F4,self.end+16)
        records=[]
        reset = 0x468940 if self.edition == "game" else 0x468740
        def stub(pc):
            sp=self.u.reg_read(UC_X86_REG_ESP)
            if pc == reset or pc == self.end+16:
                self.u.reg_write(UC_X86_REG_EAX,1000 if pc == self.end+16 else 0)
                self.u.reg_write(UC_X86_REG_EIP,self.r(sp)); self.u.reg_write(UC_X86_REG_ESP,sp+4)
            elif pc == target:
                records.append([self.r(sp+4+4*i) for i in range(len(expected_args))]);self.u.emu_stop()
        self.run(self.layout["dispatch"], hook=stub, frame=True)
        assert records == [expected_args], records
        return dict(context=operation, callee=hex(target), arguments=expected_args)

    def transparent_panel(self):
        root=self.scratch+4096
        fn=0x4AAAA0 if self.edition=="game" else 0x4AA910
        isgame=0x46BE30 if self.edition=="game" else 0x46BC50
        self.w(root,1);self.w(root+8,1505);self.w(root+12,56)
        self.w(root+16,55);self.w(root+20,320);self.w(root+24,0x1000)
        self.w(root+36,0);self.w(root+44,130)
        def stub(pc):
            if pc==isgame:
                sp=self.u.reg_read(UC_X86_REG_ESP)
                self.u.reg_write(UC_X86_REG_EAX,1);self.u.reg_write(UC_X86_REG_ESP,sp+4)
                self.u.reg_write(UC_X86_REG_EIP,self.r(sp))
        cases=[]
        for x,y,hit in ((1505,56,1),(1559,375,1),(1504,56,0),(1560,56,0),(1505,376,0)):
            self.w(self.layout["input"]+16,x);self.w(self.layout["input"]+20,y)
            self.run(fn,this=root,hook=stub)
            assert self.r(root+116)==0 and self.r(root+120)==hit
            cases.append(dict(x=x,y=y,hit=hit))
        return dict(rectangle=[1505,56,55,320],pixel_data="unused by original hit-test",
                    transparent_panel_blocks_world="PASS",boundaries=cases)

    def wall(self):
        ex=self.edition=="game";delta=0 if ex else 0x210
        base=0x2BB8C5C if ex else 0x212577C
        fn=0x438280-delta
        projection=0x4E6B20 if ex else 0x4E6770
        cost=0x4582D0 if ex else 0x458080
        validate=0x503730 if ex else 0x503380
        preview=0x4F9930 if ex else 0x4F9580
        self.w(0x248CF80 if ex else 0x1A55F30,0)
        self.w(0x248D0A4 if ex else 0x1A56054,0)
        self.w(base,25);self.w(self.layout["map"],1)
        self.w((0x2BB8C44 if ex else 0x2125764),0)
        events=[];invalid=False
        def ret(nargs,value=0):
            sp=self.u.reg_read(UC_X86_REG_ESP)
            self.u.reg_write(UC_X86_REG_EAX,value)
            self.u.reg_write(UC_X86_REG_ESP,sp+4+4*nargs)
            self.u.reg_write(UC_X86_REG_EIP,self.r(sp))
        def stub(pc):
            if pc==projection: ret(0)
            elif pc==cost: ret(2,77)
            elif pc==validate:
                self.w(base+0x38,int(invalid));ret(6)
            elif pc==preview: ret(6)
            elif pc==self.d["queue"]:
                sp=self.u.reg_read(UC_X86_REG_ESP)
                events.append((self.r(sp+4),[self.r(self.d["payload"]+o) for o in range(0,24,4)]))
                ret(1)
        def step(x,y,held,up):
            self.w(self.layout["map"]+4,x);self.w(self.layout["map"]+8,y)
            for off,v in ((0x34,0),(0x40,held),(0x28,up)):
                self.w(self.layout["input"]+off,v)
            self.run(fn,hook=stub)
            assert self.u.reg_read(UC_X86_REG_ESP)==self.stack+4
        step(80,95,0,0)
        assert [self.r(base+o) for o in (0x28,0x2C,0x30,0x34)]==[80,95,80,95]
        step(90,100,1,0)
        assert [self.r(base+o) for o in (0x28,0x2C,0x30,0x34)]==[80,95,90,100]
        assert not events
        invalid=True;step(90,100,1,1);assert not events
        invalid=False;step(90,100,1,1)
        assert events==[(25,[80,95,90,100,25,77])],events
        return dict(begin=[80,95,80,95],drag=[80,95,90,100],
                    no_order_before_release="PASS",invalid_placement_no_order="PASS",
                    commit=events[0],stdcall_stack="PASS")

    def ability(self):
        ex=self.edition=="game";delta=0 if ex else 0x210
        player_record=0x122E070 if ex else 0x11E3D00
        # Discover the address from the original instruction, not a guessed
        # image-wide relocation: Classic and Extreme have different layouts.
        md=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_32);md.detail=True
        ins=next(md.disasm(bytes(self.u.mem_read(0x4352EA-delta,7)),0x4352EA-delta))
        player_record=ins.operands[0].mem.disp
        self.w(player_record+14836,0)
        self.w(self.layout["map"]+0x70,100)
        results=[]
        for invalid in (1,0):
            records=[]
            def hook(pc):
                if pc==0x4352EA-delta:
                    self.u.mem_write(self.stack+0x12,bytes([invalid,0]))
                elif pc==self.layout["ability"]:
                    self.u.reg_write(UC_X86_REG_EIP,self.layout["ability_do"])
                elif pc==0x435723-delta:
                    records.append("blocked");self.u.emu_stop()
                elif pc==self.d["queue"]:
                    sp=self.u.reg_read(UC_X86_REG_ESP)
                    records.append((self.r(sp+4),[self.r(self.d["payload"]+o) for o in (0,4,8)]))
                    self.u.emu_stop()
            self.u.reg_write(UC_X86_REG_EAX,1);self.u.reg_write(UC_X86_REG_EBX,13)
            self.run(0x4352EA-delta,this=14836,hook=hook,frame=True)
            assert records==(["blocked"] if invalid else [(119,[13,100,1])]),records
            results.append(dict(invalid=bool(invalid),result=records))
        return results

    def cancel_without_mouse_pick(self):
        ex=self.edition=="game";delta=0 if ex else 0x210
        build=0x2BB8C5C if ex else 0x212577C
        mode=0x2BB8D5C if ex else 0x212587C
        source=ROOT/f"generated/{self.edition}-x86/recomp_0010.c"
        patched,_=apply_touch_hooks(source.read_bytes(),self.edition)
        for key in ("build_cancel","cancel"):
            marker=b"/* 0x%08X:"%self.layout[key]
            at=patched.index(marker)
            assert b"if (stronghold_touch_cancel()) goto L_%08X;"%self.layout[key+"_do"] in patched[max(0,at-600):at]
        cases=[]
        for inside in (False,True):
            for early in (False,True):
                self.w(build,25);self.w(self.layout["map"],int(inside))
                self.w(self.layout["input"]+0x3c,0)
                self.u.reg_write(UC_X86_REG_EBP,0)
                entry=0x434994-delta;guard=entry if early else 0x4349BE-delta
                def building(pc):
                    if pc==guard:self.u.reg_write(UC_X86_REG_EIP,self.layout["build_cancel_do"])
                    elif pc in (0x4349B0-delta,0x4349DB-delta):self.u.emu_stop()
                self.run(entry,hook=building)
                assert self.r(build)==(0 if early or inside else 25)
                cases.append(dict(kind="building",mouse_in_world=inside,early_native_guard=early,
                                  pending_building=self.r(build)))
                self.w(build,0);self.w(mode,4)
                self.w(0x26027BC if ex else 0x1B6F2DC,0)
                self.w(0x2BB8EA4 if ex else 0x21259C4,10)
                self.w(self.d["units"]+16,0)
                self.u.reg_write(UC_X86_REG_EBX,61);self.u.reg_write(UC_X86_REG_EBP,1)
                entry=0x4350B4-delta;guard=self.layout["cancel"] if early else 0x4350D0-delta
                def ability(pc):
                    if pc==entry:
                        self.u.reg_write(UC_X86_REG_EFLAGS,2 if inside else 0x42)
                    if pc==guard:self.u.reg_write(UC_X86_REG_EIP,self.layout["cancel_do"])
                    elif pc==self.d["clear"]:
                        sp=self.u.reg_read(UC_X86_REG_ESP)
                        self.u.reg_write(UC_X86_REG_ESP,sp+4);self.u.reg_write(UC_X86_REG_EIP,self.r(sp))
                    elif pc in (0x437C53-delta,0x435159-delta):self.u.emu_stop()
                self.run(entry,hook=ability)
                assert self.r(mode)==(0 if early or inside else 4)
                cases.append(dict(kind="ability",mouse_in_world=inside,early_native_guard=early,
                                  pending_mode=self.r(mode)))
        return dict(scope="Original cancellation stores and control flow, only unit-clear leaf stubbed. Early native guards bypass stale desktop pick condition; no guest mouse writes.",cases=cases)

    def granary_ration(self):
        ex=self.edition=="game"
        callback=0x465CB0 if ex else 0x465A10
        node=0x5FEDA8 if ex else 0x5FEE98
        group=node-160
        selected=0x1163E08 if ex else 0x11639A8
        variant=0x2BB8EA8 if ex else 0x21259C8
        game_mode=0x2BB8EE8 if ex else 0x2125A08
        rate=0x122BE78 if ex else 0x1199258
        stamp=0x122BEA4 if ex else 0x1199284
        assert self.r(group)==0x1000000 and self.r(group+20)==callback
        assert self.r(node)==0x2000003 and self.r(node+24)==1
        self.w(game_mode,2);self.w(self.d["player"],1)
        self.w(0x59F1F4,self.end+16)
        cases=[]
        for ration in range(5):
            self.w(selected,13);self.w(variant,4);events=[]
            def hook(pc):
                if pc in (self.end+16,self.d["queue"]):
                    sp=self.u.reg_read(UC_X86_REG_ESP)
                    if pc==self.d["queue"]:
                        events.append((self.r(sp+4),self.r(self.d["payload"])))
                    self.u.reg_write(UC_X86_REG_EAX,777)
                    self.u.reg_write(UC_X86_REG_ESP,sp+(8 if pc==self.d["queue"] else 4))
                    self.u.reg_write(UC_X86_REG_EIP,self.r(sp))
            self.run(callback,(ration,),hook=hook)
            assert events==[(35,ration)] and self.r(rate+14836)==ration
            assert self.r(stamp+14836)==777 and self.r(selected)==13 and self.r(variant)==4
            assert self.u.reg_read(UC_X86_REG_ESP)==self.stack+4
            cases.append(dict(ration=ration,original_command=35,payload=ration,
                              selected_building=13,stored_HUD_variant=4,cdecl_stack="PASS"))
        return dict(original_callback=hex(callback),half_ration_descriptor=hex(node),
                    source="Exact live Classic87d9 PID21367 descriptor; equivalent original Extreme group callback and records verified.",
                    callback_preserves_granary_selection=True,cases=cases)

    def inspection_ui_mouse(self):
        """Original type-0 inspection callback must not consume a UI edge.

        The ration callback itself preserves selection. The *earlier* generic
        callback in that same owner table requests state14 on any desktop
        Ldown over a stale world pick, closing state16 without deselecting.
        Execute the real request stores, stopping before time/UI rendering.
        """
        ex=self.edition=="game"
        entry,end=self.layout["inspection_buttons"]
        table=0x5FC4B8 if ex else 0x5FC5A8
        game=0x2BB8E80 if ex else 0x21259A0
        mapva=0x2DD78A0 if ex else 0x23443C0
        selected=0x1163E08 if ex else 0x11639A8
        setter=0x46B610 if ex else 0x46B430
        setter_clock=setter+0x1E
        source=ROOT/f"generated/{self.edition}-x86"/("recomp_0012.c" if ex else "recomp_0013.c")
        patched,_=apply_touch_hooks(source.read_bytes(),self.edition)
        body=patched.split(f"void sub_{entry:08X}(void) {{".encode(),1)[1].split(b"\n}\n",1)[0]
        for offset in (0x34,0x3C):
            token=f"stronghold_touch_legacy(MEM32(0x{self.layout['input']+offset:X}))".encode()
            assert token in body, (self.edition,"inspection raw mouse predicate remains",offset)
        assert self.r(table)==0 and self.r(table+20)==entry
        predicates={entry+0x26:self.layout["input"]+0x3C,
                    entry+0xC0:self.layout["input"]+0x34}
        md=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_32)
        decoded={i.address:i for i in md.disasm(bytes(self.u.mem_read(entry,end-entry)),entry)}
        for pc,address in predicates.items():
            assert decoded[pc].mnemonic=="cmp" and hex(address) in decoded[pc].op_str
        cases=[]
        for button in (0x34,0x3C):
            for inside in (0,1):
                for native in (False,True):
                    self.w(game+12,16);self.w(game+16,4);self.w(game+24,16);self.w(game+4,0)
                    self.w(game+40,4);self.w(selected,13);self.w(mapva,inside)
                    self.w(self.layout["input"]+0x34,button==0x34)
                    self.w(self.layout["input"]+0x3C,button==0x3C)
                    updates=[];requested=[]
                    def hook(pc):
                        if pc==0x40BEE0:
                            updates.append(pc);sp=self.u.reg_read(UC_X86_REG_ESP)
                            self.u.reg_write(UC_X86_REG_EAX,0)
                            self.u.reg_write(UC_X86_REG_EIP,self.r(sp))
                            self.u.reg_write(UC_X86_REG_ESP,sp+16)
                        elif pc in predicates and native:
                            # Exact existing helper: enabled native input reads
                            # zero while preserving all original guest fields.
                            self.u.reg_write(UC_X86_REG_EFLAGS,0x42)
                            self.u.reg_write(UC_X86_REG_EIP,pc+decoded[pc].size)
                        elif pc==setter_clock:
                            requested.append((self.r(game+24),self.r(game+4)))
                            self.u.emu_stop()
                    self.run(entry,hook=hook)
                    closes=not native and (button==0x3C or inside)
                    assert updates==[0x40BEE0]
                    assert requested==([(14,0 if button==0x3C else 100)] if closes else [])
                    assert self.r(game+24)==(14 if closes else 16)
                    assert self.r(selected)==13 and self.r(game+40)==4
                    assert self.r(self.layout["input"]+button)==1
                    if not closes:assert self.u.reg_read(UC_X86_REG_ESP)==self.stack+4
                    cases.append(dict(button="Ldown" if button==0x34 else "Rdown",
                                      stale_world_pick=inside,native_predicate=native,
                                      requested_UI_state=self.r(game+24),selected_building=13,
                                      original_update_preserved=True,guest_input_unchanged=True))
        # The semantic world tap uses the same original thiscall setter with
        # the original desktop Ldown arguments, rather than restoring raw
        # mouse leakage or writing any game/UI state directly from the host.
        self.w(game+12,16);self.w(game+24,16);self.w(game+4,0);self.w(selected,13)
        mouse_before=bytes(self.u.mem_read(self.layout["input"],0x60))
        semantic=[]
        def world_request(pc):
            if pc==setter_clock:
                semantic.append((self.r(game+24),self.r(game+4)))
                self.u.emu_stop()
        self.run(setter,(14,100),this=game,hook=world_request)
        assert semantic==[(14,100)] and self.r(selected)==13
        assert bytes(self.u.mem_read(self.layout["input"],0x60))==mouse_before
        # Verify the original setter's actual stack cleanup independently of
        # its request prefix (the rest dispatches other original UI handlers).
        tail=0x46B819 if not ex else 0x46B9F9
        tail_bytes=bytes(self.u.mem_read(tail,16))
        assert b"\xC2\x08\x00" in tail_bytes
        return dict(original_type0_descriptor=hex(table),original_callback=hex(entry),
                    predicates=[hex(p) for p in predicates],
                    native_world_request=dict(original_thiscall_setter=hex(setter),self=hex(game),
                                              arguments=[14,100],callee_stack_cleanup=8,
                                              requested_UI_state=14,guest_mouse_unchanged=True),
                    scope="Real original callback and UI-state request stores. Building update leaf stubbed; stop before setter clock/render. Old raw click reproduces inspection16->14 with selected13 unchanged; native wrapper prevents it and legacy control flow remains original.",
                    cases=cases)

    def ui_hover(self):
        ex=self.edition=="game";delta=0 if ex else 0x3C0
        fn=0x4F5000-delta;node=self.scratch+4096;owner=self.scratch+8192
        input_flags={self.layout["input"]+o for o in (0x28,0x30,0x34,0x3C,0x40,0x48)}
        md=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_32);md.detail=True
        predicates={i.address:i for i in md.disasm(bytes(self.u.mem_read(fn,0x740)),fn)
                    if any(o.type==capstone.CS_OP_MEM and not o.mem.base and not o.mem.index and o.mem.disp in input_flags
                           for o in i.operands)}
        self.w(0x59F1F4,self.end+16)
        result=[]
        for kind,offset in ((2,0x34),(2,0x40),(10,0x28)):
            for guarded in (False,True):
                self.u.mem_write(node,bytes(80));self.u.mem_write(owner,bytes(80))
                for off,value in ((0,kind),(4,10),(8,20),(12,40),(16,30),(20,self.end+32),
                                  (24,1234),(36,1),(44,42),(68,9),(76,owner)):
                    self.w(node+off,value)
                self.w(self.layout["input"]+16,20);self.w(self.layout["input"]+20,30)
                for flag in input_flags:self.w(flag,0)
                self.w(self.layout["input"]+offset,1)
                self.w(self.layout["input"]+0x58,0) # wheel selector
                self.w(0x1168918 if ex else 0x11684B8,0)
                self.w(0x2BB8E78 if ex else 0x2125998,0)
                callbacks=[]
                def ret(value):
                    sp=self.u.reg_read(UC_X86_REG_ESP)
                    self.u.reg_write(UC_X86_REG_EAX,value)
                    self.u.reg_write(UC_X86_REG_ESP,sp+4)
                    self.u.reg_write(UC_X86_REG_EIP,self.r(sp))
                def hook(pc):
                    if pc==self.end+16:ret(1000)
                    elif pc==self.end+32:
                        callbacks.append(self.r(self.u.reg_read(UC_X86_REG_ESP)+4));ret(0)
                    elif guarded and pc in predicates:
                        # Same predicate value as stronghold_touch_ui_button:
                        # cmp 0,0, leaving raw input memory untouched. All
                        # three activation branches below use this exact CMP.
                        i=predicates[pc]
                        assert i.mnemonic=="cmp" and i.operands[1].type==capstone.CS_OP_IMM and i.operands[1].imm==0
                        flags=self.u.reg_read(UC_X86_REG_EFLAGS)
                        self.u.reg_write(UC_X86_REG_EFLAGS,(flags&~0x8D5)|0x44)
                        self.u.reg_write(UC_X86_REG_EIP,pc+i.size)
                self.run(fn,this=node,hook=hook)
                assert callbacks==([] if guarded else [1234]),(kind,offset,guarded,callbacks)
                assert self.rh(node+50)==1 and self.r(owner+56)==node
                assert self.r(self.layout["input"]+offset)==1
                result.append(dict(widget_type=kind,button_offset=hex(offset),guarded=guarded,
                                   callbacks=len(callbacks),tooltip_hover="preserved"))
        return result

    def ui_description(self):
        classic = self.edition == "classic"
        tooltip, setter, expire, helper = ((0x4F6E50, 0x4F57B0, 0x4F58A0, 0xF1E4B4) if classic
                                         else (0x4F7210, 0x4F5B70, 0x4F5C60, 0xF1E914))
        state = 0x21259AC if classic else 0x2BB8E8C
        request = 0x2125A60 if classic else 0x2BB8F40
        flag = self.layout["help_flag"]
        owner, node = self.scratch + 4096, self.scratch + 8192
        self.u.mem_write(owner, bytes(128)); self.u.mem_write(node, bytes(80))
        self.w(owner + 56, node); self.w(node + 44, 10)  # Live castlebar building86 tooltip.
        self.w(state, 14); self.w(state + 4, 10); self.w(0x59F1F4, self.end + 16)
        md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32); md.detail = True
        predicates = {i.address: i for i in md.disasm(bytes(self.u.mem_read(tooltip, 0x4F6)), tooltip)
                      if any(o.type == capstone.CS_OP_MEM and not o.mem.base and not o.mem.index and
                             o.mem.disp == flag for o in i.operands)}
        assert len(predicates) == 10
        source = ROOT / f"generated/{self.edition}-x86" / ("recomp_0051.c" if classic else "recomp_0049.c")
        patched, _ = apply_touch_hooks(source.read_bytes(), self.edition)
        body = patched.split(f"void sub_{tooltip:08X}(void) {{".encode(), 1)[1].split(b"\nvoid ", 1)[0]
        wrapped = f"stronghold_touch_ui_help(MEM32(0x{flag:X}))".encode()
        assert body.count(wrapped) == 10
        main_source = ROOT / f"generated/{self.edition}-x86" / ("recomp_0073.c" if classic else "recomp_0071.c")
        patched, _ = apply_touch_hooks(main_source.read_bytes(), self.edition)
        branch_hook = b"if (stronghold_touch_ui_redraw()) goto L_%08X;" % self.layout["ui_full"]
        assert branch_hook in patched

        def clock(held):
            def hook(pc):
                if pc == self.end + 16:
                    sp = self.u.reg_read(UC_X86_REG_ESP)
                    self.u.reg_write(UC_X86_REG_EAX, 1000)
                    self.u.reg_write(UC_X86_REG_EIP, self.r(sp))
                    self.u.reg_write(UC_X86_REG_ESP, sp + 4)
                elif held and pc in predicates:
                    # Match ui_help(0)==1 without modifying the original flag.
                    i = predicates[pc]
                    assert i.mnemonic == "cmp" and self.u.reg_read(UC_X86_REG_EBP) == 0
                    self.u.reg_write(UC_X86_REG_EFLAGS, self.u.reg_read(UC_X86_REG_EFLAGS) & ~0x8D5)
                    self.u.reg_write(UC_X86_REG_EIP, pc + i.size)
            return hook

        gates = []
        for configured in (0, 1):
            for held in (False, True):
                self.u.mem_write(helper, bytes(64)); self.w(flag, configured)
                self.run(tooltip, this=owner, hook=clock(held))
                assert self.r(helper) == (2 if configured or held else 0)
                assert self.r(flag) == configured
                if configured or held:
                    assert [self.r(helper + i * 4) for i in range(6)] == [2, 8, 10, 86, 50, 0]
                gates.append(dict(configured_help=configured, held=held, help_type=self.r(helper), config_unchanged=True))
        lifetimes = []
        self.w(flag, 1)
        for refresh in (False, True):
            self.u.mem_write(helper, bytes(64))
            self.run(setter, args=(2, 8, 10, 86, 50, -1), this=helper, hook=clock(False))
            frames = []
            for _ in range(4):
                self.run(expire, this=helper, hook=clock(False))
                if refresh: self.run(tooltip, this=owner, hook=clock(True))
                frames.append(self.r(helper))
            assert frames == ([2, 2, 2, 2] if refresh else [2, 0, 0, 0])
            lifetimes.append(dict(refresh_each_held_frame=refresh, help_type_per_frame=frames))
        branches = []
        entry, full = self.layout["ui_redraw"] - 6, self.layout["ui_full"]
        for original_request in (0, 1, 2):
            for held in (False, True):
                self.w(request, original_request); self.u.reg_write(UC_X86_REG_EBP, 0); reached = []
                def branch(pc):
                    if held and pc == self.layout["ui_redraw"]:
                        self.u.reg_write(UC_X86_REG_EIP, full)
                    elif pc in (entry + 12, full):
                        reached.append(pc); self.u.emu_stop()
                self.run(entry, hook=branch)
                assert reached == [full if original_request or held else entry + 12]
                assert self.r(request) == original_request
                branches.append(dict(original_request=original_request, held=held,
                                     full_HUD=reached[0] == full, request_unchanged=True))
        return dict(scope="Original tooltip, HELP setter/expiry and WinMain branch executed; only timeGetTime stubbed. Native predicates simulated at their exact hook instructions; actual emitted hook expressions checked.",
                    generated_help_flag_reads=10, configured_help_and_hold=gates,
                    original_HELP_lifetime=lifetimes, original_full_HUD_branch=branches)


def main():
    result={}
    for edition in EDITIONS:
        o=Original(edition)
        selection=o.selection()
        lord=o.lord_selection()
        fortification=o.owned_fortification()
        fortification_eligibility=o.fortification_eligibility()
        move=o.payload(o.d["move"],(7,80,95,0,1),17,[7,80,95,0,1])
        attack=o.payload(o.d["target"],(7,16,4,1234),36,[7,16,4,1234,1])
        # The real contextual dispatcher keeps the different command routes;
        # the native hook only enters it after the original validity check.
        dispatched=o.dispatch(3,o.d["target"],[7,16,4,1234])
        interaction=o.dispatch(4,0x537FB0 if edition=="game" else 0x537B60,[7,17,4,1234,0])
        transparent=o.transparent_panel();wall=o.wall();ability=o.ability();hover=o.ui_hover()
        box=o.box_receiver()
        mask=o.ui_pixel_mask()
        description=o.ui_description()
        cancel=o.cancel_without_mouse_pick()
        ration=o.granary_ration()
        inspection=o.inspection_ui_mouse()
        result[edition]=dict(image=o.d["name"],sha256=o.sha,selection=selection,
                             lord_selection=lord,
                             owned_fortification=fortification,
                             fortification_eligibility=fortification_eligibility,
                             move=move,attack=attack,context_dispatch=dispatched,
                             specific_interaction_dispatch=interaction,
                             transparent_panel=transparent,wall=wall,ability=ability)
        result[edition]["ui_hover"]=hover
        result[edition]["box_receiver"]=box
        result[edition]["ui_pixel_mask"]=mask
        result[edition]["ui_description"]=description
        result[edition]["cancel_without_mouse_pick"]=cancel
        result[edition]["granary_ration"]=ration
        result[edition]["inspection_ui_mouse"]=inspection
        print(edition, "original touch paths / BOX receiver TGX preservation / UI mask / held description HELP lifetime, full-HUD branch and unchanged config PASS",flush=True)
    (ROOT/"generated/touch-original-proof.json").write_text(json.dumps(result,indent=2)+"\n")


if __name__ == "__main__": main()
