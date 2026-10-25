"""Run the real DirectPlay COM transport and native touch checks on ARM64 Android.

Close the game on this device first: the LAN checks bind the game's actual ports.
"""
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
    adb = [str(Path.home() / 'AppData/Local/Android/Sdk/platform-tools/adb.exe'), '-s', args.serial]
    results = []
    for edition in ('classic', 'extreme'):
        for test in ('dplay', 'touch'):
            binary = ROOT / f'build/{test}-{edition}-selfcheck'
            command = [str(CLANG), '--target=aarch64-linux-android30', '-O2', '-std=gnu11',
                       '-Iandroid/runtime', '-Iandroid/host']
            if edition == 'classic':
                command += ['-DSTRONGHOLD_CLASSIC']
            command += [f'tests/{test}_selfcheck.c', '-o', str(binary), '-lm', '-Wl,-z,max-page-size=16384']
            subprocess.run(command, cwd=ROOT, check=True)
            target = '/data/local/tmp/' + binary.name
            subprocess.run([*adb, 'push', str(binary), target], check=True, capture_output=True)
            try:
                subprocess.run([*adb, 'shell', 'chmod', '755', target], check=True, capture_output=True)
                run = subprocess.run([*adb, 'shell', target], text=True, capture_output=True, check=True, timeout=30)
                assert 'PASS' in run.stdout and 'FAIL' not in run.stdout
                results.append(dict(edition=edition, test=test, serial=args.serial, stdout=run.stdout,
                                    sha256=hashlib.sha256(binary.read_bytes()).hexdigest()))
                print(edition, test, run.stdout.strip(), flush=True)
            finally:
                subprocess.run([*adb, 'shell', 'rm', target], capture_output=True, check=True)
    (ROOT / 'generated/lan-touch-selfcheck.json').write_text(json.dumps(results, indent=2) + '\n')


if __name__ == '__main__':
    main()
