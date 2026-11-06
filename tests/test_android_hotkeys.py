"""Execute original Alt+R and Ctrl+Alt/Alt bookmark handlers in both pinned PEs."""
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from test_touch import Original
from test_game_text import Editor
from unicorn.x86_const import UC_X86_REG_EAX, UC_X86_REG_EIP, UC_X86_REG_ESP


def main():
    results = []
    hd = Editor('stronghold'); transitions = []; actions = []
    def hd_hook(pc):
        if pc==0x49314F: hd.u.emu_stop()
        elif pc in (0x45BEB0,0x48E2F0,0x4F3990):
            sp=hd.u.reg_read(UC_X86_REG_ESP)
            if pc==0x45BEB0: transitions.append((hd.r(sp+4),hd.r(sp+8)));pops=2
            elif pc==0x48E2F0: actions.append(hd.r(sp+4));pops=2
            else: actions.append('animals');pops=0
            hd.u.reg_write(UC_X86_REG_ESP,sp+4+pops*4);hd.u.reg_write(UC_X86_REG_EIP,hd.r(sp))
    hd.w(0x1503CE4,0xffffffff);hd.w(0x1503D84,1);hd.w(0x1505670,1)
    hd.run(0x4394E0,(25,),hook=hd_hook)
    assert transitions==[(1002,0)],transitions
    hd.w(0x1503D38,1002);hd.h(0xBF6452,0)
    for expected in (1,0):
        hd.run(0x4923D0,(0x10010,0x104,0xDE,1),hook=hd_hook)
        assert hd.rh(0xBF6452)==expected
    hd.w(0x1503D38,17)
    for key in (0xBC,ord('E')):hd.run(0x4923D0,(0x10010,0x104,key,1),hook=hd_hook)
    assert actions==[31,'animals'],actions
    results.append(dict(edition='stronghold',sha256=hd.sha,editor_resources_round_trip=True,map_type_dialog=31,animals_handler=True))
    for edition in ('classic', 'game'):
        o = Original(edition)
        ex = edition == 'game'
        game = 0x2BB8E80 if ex else 0x21259A0
        ctrl = 0xF5FD94 if ex else 0xF5F934
        marks = 0x124C78C if ex else 0x11B9B6C
        table = 0x2E25E80 if ex else 0x23929A0
        radar = 0x2BBBDB0 if ex else 0x21288D0
        wndproc = 0x4B3410 if ex else 0x4B3280
        recalled = []

        def ret(n):
            u = o.u
            stack = u.reg_read(UC_X86_REG_ESP)
            u.reg_write(UC_X86_REG_EAX, 1)
            u.reg_write(UC_X86_REG_ESP, stack + 4 + n * 4)
            u.reg_write(UC_X86_REG_EIP, o.r(stack))

        def hook(pc):
            if pc == (0x4B4510 if ex else 0x4B4380):
                o.u.emu_stop()  # Command finished; no Windows default window procedure is needed.
            elif pc == (0x46BE30 if ex else 0x46BC50):
                ret(0)
            elif pc == (0x4E6B70 if ex else 0x4E67C0):
                recalled.append(o.r(o.u.reg_read(UC_X86_REG_ESP) + 4))
                ret(1)

        o.w(game + 12, 14)
        o.w(ctrl + 4, 0)
        o.w(ctrl + 8, 1)
        for offset in (0x78, 0x7C, 0xAC, 0xB0):
            o.w(o.layout['map'] + offset, 0)
        o.w(0x2BB8C14 if ex else 0x2125734, 0)
        for slot in range(1, 10):
            key = ord('0') + slot
            o.w(table + 8 * 4, 12345 + slot)
            o.w(marks + key * 4, 0xFFFFFFFF)
            o.w(ctrl, 1)
            o.run(wndproc, (0x10010, 0x104, key, 1), hook=hook)
            assert o.r(marks + key * 4) == 12345 + slot
            o.w(ctrl, 0)
            o.run(wndproc, (0x10010, 0x104, key, 1), hook=hook)
            assert recalled[-1] == 12345 + slot
        o.w(radar, 0)
        for expected in (1, 0):
            o.run(wndproc, (0x10010, 0x104, ord('R'), 1), hook=hook)
            assert o.r(radar) == expected
        transitions = []

        def scenario_hook(pc):
            if pc == (0x46B610 if ex else 0x46B430):
                stack = o.u.reg_read(UC_X86_REG_ESP)
                transitions.append((o.r(stack + 4), o.r(stack + 8)))
                ret(2)

        o.w(0x1163EAC if ex else 0x1163A4C, 0)
        o.w(0x2BB8E2C if ex else 0x212594C, 0xFFFFFFFF)
        o.w(0x2BBA79C if ex else 0x21272BC, 0)
        o.w(0x2BBB7D0 if ex else 0x21282F0, 1)
        o.run(0x4433C0 if ex else 0x4431F0, (25,), hook=scenario_hook)
        assert transitions == [(1002, 0)], transitions
        # The original initial-tax/ration/money panel is specific to scenario editor1002.
        resource_fields = 0x124D5DE if ex else 0x11BA9BE
        o.h(resource_fields, 0)
        o.w(game + 12, 1002)
        for expected in (1, 0):
            o.run(wndproc, (0x10010, 0x104, 0xDE, 1), hook=hook)
            assert o.rh(resource_fields) == expected
        results.append(dict(edition=edition, sha256=o.sha, bookmark_slots=9,
                            bookmark_index='virtual key', radar_round_trip=True, editor_resources_round_trip=True))
    (ROOT / 'generated/android-hotkeys-proof.json').write_text(json.dumps(results, indent=2) + '\n')
    print('Original HD editor transitions/resources/type/animals; Classic/Extreme Alt+R, nine bookmarks and initial resources: PASS')


if __name__ == '__main__':
    main()
