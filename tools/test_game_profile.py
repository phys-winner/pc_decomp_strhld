"""Prove autosave fields against the original x86 writers; optionally run ARM64 checks."""
import argparse
import json
import re
import subprocess

from unicorn.x86_const import UC_X86_REG_EAX, UC_X86_REG_EIP, UC_X86_REG_ESP
from build_android import CLANG, ROOT
from test_game_text import Editor


LAYOUT = {
    'classic': (0x496C20, 0x478340, 0x58411B, 0x581664, 0x580A29, 0x581B60),
    'extreme': (0x496DB0, 0x478530, 0x58456B, 0x581AB4, 0x580E79, 0x581FB0),
    'stronghold': (0x47F2C0, 0x465C50, 0x53A0EB, 0x537600, 0x536917, 0x5363BA),
}


def writer(edition):
    e = Editor(edition)
    function, path, fopen, fwrite, fclose, free = LAYOUT[edition]
    written = []

    def ret(value=0, pop=0):
        sp = e.u.reg_read(UC_X86_REG_ESP)
        e.u.reg_write(UC_X86_REG_EAX, value)
        e.u.reg_write(UC_X86_REG_EIP, e.r(sp))
        e.u.reg_write(UC_X86_REG_ESP, sp + 4 + pop)

    def leaf(pc):
        sp = e.u.reg_read(UC_X86_REG_ESP)
        if pc == path:
            obj = e.r(sp + 4)
            e.u.mem_write(obj, bytes(28))
            e.h(obj + 4, ord('X'))
            e.w(obj + 24, 7)
            ret(obj, 8)
        elif pc == fopen:
            ret(77)
        elif pc == fwrite:
            pointer, size, count, handle = (e.r(sp + 4 + i * 4) for i in range(4))
            assert handle == 77
            written.append((pointer, size * count))
            ret(count)
        elif pc in (fclose, free):
            ret()

    e.run(function, hook=leaf)
    assert e.u.reg_read(UC_X86_REG_ESP) == e.stack + 4
    assert (e.d['editor'] + 336, 512) in written
    assert sum(size for _, size in written) == (826 if edition == 'stronghold' else 1328)
    return dict(sha256=e.sha, writer=hex(function), fields=written,
                scope='Complete original config writer; only path/CRT/filesystem leaves intercepted.')


def trail_victories(edition):
    classic = edition == 'classic'
    game = 0x21259A0 if classic else 0x2BB8E80
    results = []
    for trail in range(3):
        e = Editor(edition)
        e.w(game + 0x68, 3)
        e.w(0x1A559B0 if classic else 0x248CA00, 99)
        e.w(0xF0FBE4 if classic else 0xF10044, 1)
        e.w(game + 0x2b4c, 1)
        e.w(game + 0x2b54, trail)
        e.u.mem_write(game + 0x2f2f, b'\x01')
        e.w(0x1B6526C if classic else 0x25F874C, 1)
        e.h((0x11BC388 if classic else 0x124EFA8) + 2, 1)
        e.w(0x11B9EFC if classic else 0x124CB1C, 7)
        e.w(0x11B9F00 if classic else 0x124CB20, 1100)
        for offset, size in ((0x29b4, 200), (0x2b5c, 120), (0x2d00, 120)):
            e.u.mem_write(game + offset, b'\xff' * size)
        completed = game + (0x29b0, 0x2b58, 0x2cfc)[trail]
        position = game + (0x2b50, 0x2cf8, 0x2e9c)[trail]
        best = game + (0x29b4, 0x2b5c, 0x2d00)[trail]
        e.w(completed, 0)
        e.w(position, 0)
        # Render/audio/cleanup/UI dispatch are leaves; the actual victory and date calculation run.
        leaves = {0x408EB0: 0, (0x4D6180 if classic else 0x4D6530): 0,
                  (0x47A520 if classic else 0x47A710): 4, (0x42BFD0 if classic else 0x42C050): 0}
        saves = []

        def leaf(pc):
            if pc == LAYOUT[edition][0]:
                saves.append(pc)
            if pc in leaves or pc == LAYOUT[edition][0]:
                sp = e.u.reg_read(UC_X86_REG_ESP)
                e.u.reg_write(UC_X86_REG_EAX, 0)
                e.u.reg_write(UC_X86_REG_EIP, e.r(sp))
                e.u.reg_write(UC_X86_REG_ESP, sp + 4 + leaves.get(pc, 0))

        e.run(0x4DCEA0 if classic else 0x4DD250, hook=leaf)
        assert [e.r(p) for p in (completed, position, best)] == [1, 1, 13207]
        assert e.u.reg_read(UC_X86_REG_EIP) == e.end
        assert e.u.reg_read(UC_X86_REG_ESP) == e.stack + 4
        assert not saves, 'The original trail victory does not save the changed progress'
        results.append(dict(trail=trail, progress_addresses=[completed, position, best],
                            completed=1, position=1, best=13207, original_saves=0))
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--serial')
    args = parser.parse_args()
    results = {}
    for edition in LAYOUT:
        result = writer(edition)
        if edition != 'stronghold':
            result['victories'] = trail_victories(edition)
        if args.serial:
            binary = ROOT / f'build/game-profile-{edition}-selfcheck'
            define = {'classic': ['-DSTRONGHOLD_CLASSIC'], 'extreme': [],
                      'stronghold': ['-DSTRONGHOLD_ORIGINAL']}[edition]
            subprocess.run([str(CLANG), '--target=aarch64-linux-android28', '-O2', '-std=gnu11',
                            *define, '-Iandroid/runtime', '-Iandroid/host', 'tests/game_profile_selfcheck.c',
                            '-Wl,-z,max-page-size=16384', '-o', str(binary)], cwd=ROOT, check=True)
            remote = f'/data/local/tmp/game-profile-{edition}-selfcheck'
            adb = ['adb', '-s', args.serial]
            subprocess.run([*adb, 'push', str(binary), remote], capture_output=True, check=True)
            try:
                subprocess.run([*adb, 'shell', 'chmod', '755', remote], capture_output=True, check=True)
                run = subprocess.run([*adb, 'shell', remote], capture_output=True, text=True, check=True)
                assert 'PASS' in run.stdout
                watched = [(int(address, 16), int(size)) for address, size in
                           re.findall(r'^field ([0-9A-F]+) (\d+)$', run.stdout, re.M)]
                actual = {p for address, size in result['fields'] for p in range(address, address + size)}
                assert watched and all(set(range(address, address + size)) <= actual for address, size in watched)
                watched_bytes = {p for address, size in watched for p in range(address, address + size)}
                for victory in result.get('victories', []):
                    assert all(set(range(address, address + 4)) <= watched_bytes for address in victory['progress_addresses'])
                result['watched'] = watched
                result['device'] = args.serial
                result['native'] = run.stdout.strip()
            finally:
                subprocess.run([*adb, 'shell', 'rm', remote], capture_output=True, check=True)
        results[edition] = result
        print(f'{edition}: original writer' + (' + actual ARM64 autosave fields/guards' if args.serial else '') + ' PASS')
    (ROOT / 'generated/game-profile-proof.json').write_text(json.dumps(results, indent=2) + '\n')


if __name__ == '__main__':
    main()
