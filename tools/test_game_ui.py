"""Exercise the actual guest-thread Android settings/editor seam on ARM64."""
import argparse
import hashlib
import json
import subprocess
from pathlib import Path
from build_android import CLANG, ROOT


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--serial', required=True)
    args = parser.parse_args()
    adb = Path.home() / 'AppData/Local/Android/Sdk/platform-tools/adb.exe'
    result = []
    for edition in ('extreme', 'classic'):
        binary = ROOT / f'build/game-ui-{edition}-selfcheck'
        command = [str(CLANG), '--target=aarch64-linux-android30', '-O2', '-std=gnu11',
                   '-Iandroid/runtime', '-Iandroid/host']
        if edition == 'classic':
            command += ['-DSTRONGHOLD_CLASSIC']
        command += ['tests/game_ui_selfcheck.c', '-o', str(binary), '-Wl,-z,max-page-size=16384']
        subprocess.run(command, cwd=ROOT, check=True)
        target = '/data/local/tmp/game-ui-' + edition + '-selfcheck'
        prefix = [str(adb), '-s', args.serial]
        subprocess.run([*prefix, 'push', str(binary), target], capture_output=True, check=True)
        subprocess.run([*prefix, 'shell', 'chmod', '755', target], capture_output=True, check=True)
        run = subprocess.run([*prefix, 'shell', target], capture_output=True, text=True, check=True)
        assert 'PASS' in run.stdout
        result.append(dict(edition=edition, device=args.serial, stdout=run.stdout,
                           sha256=hashlib.sha256(binary.read_bytes()).hexdigest()))
        subprocess.run([*prefix, 'shell', 'rm', target], capture_output=True, check=True)
        print(edition, run.stdout.strip())
    (ROOT / 'generated/game-ui-selfcheck.json').write_text(json.dumps(result, indent=2) + '\n')


if __name__ == '__main__':
    main()
