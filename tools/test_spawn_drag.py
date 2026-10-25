"""Execute the original Crusader setup flag drag and slot swap in Unicorn."""
from unicorn.x86_const import UC_X86_REG_EIP, UC_X86_REG_ESP

from test_touch import Original


def check(edition):
    o = Original(edition)
    classic = edition == 'classic'
    callback = 0x4427B0 if classic else 0x442980
    positions = 0x21282A4 if classic else 0x2BBB784
    players = 0x1B65260 if classic else 0x25F8740
    allowed = 0x1A55B28 if classic else 0x248CB78
    dragged = 0xB975C0 if classic else 0xB97740
    origin = 0xFD57A0 if classic else 0xFD5C00
    input_state = 0xF69DF8 if classic else 0xF6A258
    o.w(0xB98124 if classic else 0xB982A4, 0)
    o.w(0x1163A4C if classic else 0x1163EAC, 0)
    o.w(allowed, 1)
    o.w(origin, 130)
    o.w(origin + 4, 50)
    for i in range(8):
        o.w(positions + 8 * i, 10 + i * 40)
        o.w(positions + 8 * i + 4, 20 + i * 40)
    original = bytes(range(1, 9))
    queues = []

    def queue(pc):
        if pc != o.d['queue']:
            return
        sp = o.u.reg_read(UC_X86_REG_ESP)
        assert o.r(sp + 4) == 13
        queues.append(13)
        o.u.reg_write(UC_X86_REG_EIP, o.r(sp))
        o.u.reg_write(UC_X86_REG_ESP, sp + 8)

    def event(code, x, y):
        o.w(input_state + 16, x)
        o.w(input_state + 20, y)
        o.run(callback, args=(code,), hook=queue)

    for target, enabled in ((3, True), (None, True), (3, False)):
        o.u.mem_write(players, original)
        o.w(dragged, -1)
        o.w(allowed, int(enabled))
        event(-200, 712, 73)
        assert o.r(dragged) == (1 if enabled else 0xFFFFFFFF)
        event(-201, 832 if target is not None else 0, 193 if target is not None else 0)
        expected = bytearray(original)
        if enabled and target is not None:
            expected[0], expected[target] = expected[target], expected[0]
        assert bytes(o.u.mem_read(players, 8)) == bytes(expected)
        assert o.r(dragged) == 0xFFFFFFFF
    assert queues == [13]
    print(edition, 'original setup flag press/release, slot swap, outside drop and host permission PASS')


if __name__ == '__main__':
    for edition in ('classic', 'game'):
        check(edition)
