"""Execute hash-pinned HD area callbacks and RMB cancellation in original x86."""
import json
from unicorn.x86_const import UC_X86_REG_EAX, UC_X86_REG_ESP, UC_X86_REG_EIP
from test_game_text import Editor
from build_android import ROOT


def proof():
    original = Editor('stronghold')
    u = original.u
    queue, preview = [], []
    pointer = [100, 110]
    original.w(0x554204, original.end + 16)
    original.w(0x5541CC, original.end + 32)

    def ret(value=0, pop=0):
        sp = u.reg_read(UC_X86_REG_ESP)
        u.reg_write(UC_X86_REG_EAX, value)
        u.reg_write(UC_X86_REG_EIP, original.r(sp))
        u.reg_write(UC_X86_REG_ESP, sp + 4 + pop)

    def leaves(pc):
        # Only coordinate/raster/economy leaves are fixtures. The original
        # down/held/up gates, start/end stores, cancel and queue calls execute.
        if pc in (0x4B3530, 0x4B3580):
            ret()
        elif pc == original.end + 16:
            ret(1000)
        elif pc == original.end + 32:
            sp = u.reg_read(UC_X86_REG_ESP)
            original.w(original.r(sp + 4), pointer[0])
            original.w(original.r(sp + 4) + 4, pointer[1])
            ret(1, 4)
        elif pc == 0x44AAD0:
            ret(100, 8)
        elif pc in (0x4C8ED0, 0x4C0250):
            sp = u.reg_read(UC_X86_REG_ESP)
            preview.append((pc, [original.r(sp + 4 + 4 * n) for n in range(6)]))
            original.w(0x1503B5C, 0)
            ret(pop=24)
        elif pc == 0x4C7870:
            ret(1, 12)  # Original demolition accepts this target.
        elif pc == 0x4BE8B0:
            ret(pop=4)
        elif pc == 0x475D40:
            sp = u.reg_read(UC_X86_REG_ESP)
            queue.append((original.r(sp + 4), [original.r(0xF9FFE8 + 4 * n) for n in range(6)]))
            ret(pop=4)

    def reset(kind):
        u.mem_write(0xABC368, bytes(0x200))
        for address in (0xF67700, 0xF67814, 0xBCAF70, 0x1503B0C, 0xABC4E8,
                        0x1503BD4, 0x1503BD8, 0x1503B5C):
            original.w(address, 0)
        original.w(0x1503D84, 1)
        original.w(0x1503D38, 14)
        original.w(0x1503B24, kind)
        original.w(0x1008144, 1)
        original.w(0x16E9358, 1)
        original.w(0xB182D0, 1200)
        original.w(0xB182D4, 600)
        queue.clear()
        preview.clear()

    def frame(down=0, held=0, up=0, right=0, x=100, y=110):
        pointer[:] = [x, y]
        original.h(0xABC368 + 0x1E8, x)
        original.h(0xABC368 + 0x1EA, y)
        u.mem_write(0xABC368 + 0x1EE, bytes([held | (16 if right else 0)]))
        original.run(0x459250, this=0xABC368, hook=leaves)
        assert (original.r(0xABC39C), original.r(0xABC3A8), original.r(0xABC390)) == (down, held, up)
        assert original.r(0xABC3A4) == right
        for address, value in ((0x16E935C, x), (0x16E9360, y)):
            original.w(address, value)

    walls = []
    for kind in (25, 46, 27, 26):
        reset(kind)
        frame(down=1, held=1)
        original.run(0x430830, hook=leaves)
        assert not queue and (original.r(0x1503B4C), original.r(0x1503B50)) == (100, 110)
        frame(held=1, x=104, y=115)
        original.run(0x430830, hook=leaves)
        assert not queue and (original.r(0x1503B54), original.r(0x1503B58)) == (104, 115)
        assert preview[-1][1][1:5] == [100, 110, 104, 115]
        frame(up=1, x=104, y=115)
        original.run(0x430830, hook=leaves)
        assert len(queue) == 1 and queue[0][0] == 25 and queue[0][1][:5] == [100, 110, 104, 115, kind]
        committed = list(queue)
        reset(kind)
        frame(down=1, held=1)
        original.run(0x430830, hook=leaves)
        frame(held=1, right=1, x=104, y=115)
        original.run(0x42D630, hook=leaves)  # Real RMB callback, before any left release.
        assert original.r(0x1503B24) == 0
        original.run(0x430830, hook=leaves)
        frame(up=1, x=104, y=115)
        original.run(0x430830, hook=leaves)
        assert not queue
        walls.append(dict(type=kind, held_preview='PASS', committed_queue=committed, rmb_before_release_no_build='PASS'))

    reset(25)
    frame(down=1, held=1)
    original.run(0x430830, hook=leaves)
    frame(up=1)
    original.run(0x430830, hook=leaves)
    assert queue  # Releasing left first irreversibly queues construction.

    reset(25)
    frame(down=1, held=1)
    original.run(0x430830, hook=leaves)
    original.w(0xF67700, 1)  # Modal gate swallows the first real right-down.
    frame(held=1, right=1)
    original.run(0x42D630, hook=leaves)
    original.run(0x430830, hook=leaves)
    assert original.r(0x1503B24) == 25 and not queue
    original.w(0xF67700, 0)
    frame(held=1)  # Sample right-up with left still held.
    frame(held=1, right=1)
    original.run(0x42D630, hook=leaves)
    assert original.r(0x1503B24) == 0
    original.run(0x430830, hook=leaves)
    frame(up=1)
    original.run(0x430830, hook=leaves)
    assert not queue

    demolition = []
    for stage, down, held, up in (('down', 1, 1, 0), ('held', 0, 1, 0), ('idle', 0, 0, 0)):
        reset(39)
        if stage == 'held':
            original.w(0xABC368 + 0x40, 1)
        frame(down=down, held=held, up=up)
        original.run(0x431020, hook=leaves)
        assert [item[0] for item in queue] == ([97] if stage == 'down' else []), (stage, queue)
        demolition.append(dict(stage=stage, original_queue=list(queue)))
    return dict(status='PASS', sha256=original.sha,
                scope='Unmodified original459250 input sampler and430830/431020/42D630; clock/cursor, ground picker, preview raster, affordability and demolition target-validation leaves stubbed; real raw button sampling, phase gates, anchors, cancel stores and queue decisions run.',
                walls=walls, release_before_cancel_queues='PASS', demolition=demolition,
                modal_swallowed_rmb_retry_before_left_release='PASS',
                demolition_held_preview_safe=False, demolition_down_queue_pc='0x43128F')


if __name__ == '__main__':
    result = proof()
    (ROOT / 'generated/original-aim-proof.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))
