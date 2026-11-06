"""Generate checked EXE normalization deltas and language metadata from owned GOG inputs.

No EXE or TEX is stored in the source tree. Android reconstructs the pinned
engine image in its private cache, retaining the selected installation's TEX.
"""
import argparse
import gzip
import hashlib
import json
from pathlib import Path
import struct

ROOT = Path(__file__).resolve().parents[1]
CATALOG = ROOT / 'config/compatibility.json'
OUT = ROOT / 'generated/compatibility'
JAVA = ROOT / 'android/app/src/org/stronghold/port/GameCompatibility.java'
NAMES = {'classic': 'Stronghold Crusader.exe', 'extreme': 'Stronghold_Crusader_Extreme.exe', 'stronghold': 'Stronghold.exe'}


def make_patch(source, target):
    count = max(len(source), len(target))
    if count > 16 * 1024 * 1024:
        raise ValueError('EXE exceeds the normalization size limit')
    a, b = source.ljust(count, b'\0'), target.ljust(count, b'\0')
    data = bytes(x ^ y for x, y in zip(a, b))
    header = struct.pack('<4sIII32s32s', b'SHD1', len(source), len(target), count,
                         hashlib.sha256(source).digest(), hashlib.sha256(target).digest())
    return header + gzip.compress(data, mtime=0)


def apply_patch(source, patch):
    if len(patch) < 80:
        raise ValueError('Truncated normalization header')
    magic, before_size, after_size, count, before, after = struct.unpack_from('<4sIII32s32s', patch)
    if (magic != b'SHD1' or count != max(before_size, after_size) or count > 16 * 1024 * 1024
            or len(source) != before_size or hashlib.sha256(source).digest() != before):
        raise ValueError('Normalization input mismatch')
    data = gzip.decompress(patch[80:])
    if len(data) != count:
        raise ValueError('Normalization delta length mismatch')
    result = bytes(x ^ y for x, y in zip(source.ljust(count, b'\0'), data))[:after_size]
    if hashlib.sha256(result).digest() != after:
        raise ValueError('Normalization output mismatch')
    return result


def java_source(catalog):
    quote = lambda value: json.dumps(value, ensure_ascii=True)
    lines = ['package org.stronghold.port;', '',
             '// Generated from config/compatibility.json by tools/prepare_compat.py.',
             'final class GameCompatibility {',
             '    static final class Executable {',
             '        final String edition, sha256, target;',
             '        final int bytes, targetBytes;',
             '        Executable(String e, String s, int b, String t, int n) { edition=e; sha256=s; bytes=b; target=t; targetBytes=n; }',
             '    }', '    static final Executable[] EXECUTABLES = {']
    for edition, entries in catalog['executables'].items():
        for digest, entry in entries.items():
            lines.append(f'        new Executable({quote(edition)}, {quote(digest)}, {entry["bytes"]}, {quote(entry["target"])}, {entry["target_bytes"]}),')
    lines += ['    };', '    static Executable executable(String edition, String sha256) {',
              '        for (Executable entry : EXECUTABLES) if (entry.edition.equals(edition) && entry.sha256.equals(sha256)) return entry;',
              '        return null;', '    }', '    static final String[][] LANGUAGES = {']
    for filename, entries in catalog['languages'].items():
        for digest, entry in entries.items():
            lines.append('        {' + ', '.join(map(quote, [filename, digest, entry['tag'], entry['name']])) + '},')
    lines += ['    };', '    static String[] language(String filename, String sha256) {',
              '        for (String[] entry : LANGUAGES) if (entry[0].equals(filename) && entry[1].equals(sha256)) return entry;',
              '        return null;', '    }', '}', '']
    return '\n'.join(lines)


def prepare(inputs):
    catalog = json.loads(CATALOG.read_text(encoding='utf-8'))
    JAVA.write_text(java_source(catalog), encoding='utf-8')
    OUT.mkdir(parents=True, exist_ok=True)
    wanted = {digest: (edition, entry) for edition, entries in catalog['executables'].items()
              for digest, entry in entries.items() if digest != entry['target']}
    for file in inputs.rglob('*.exe'):
        if file.name not in NAMES.values() or file.stat().st_size > 16 * 1024 * 1024:
            continue
        source = file.read_bytes()
        digest = hashlib.sha256(source).hexdigest()
        if digest not in wanted:
            continue
        edition, entry = wanted.pop(digest)
        target = (ROOT / 'generated/input' / NAMES[edition]).read_bytes()
        if hashlib.sha256(target).hexdigest() != entry['target']:
            raise ValueError(f'{NAMES[edition]}: canonical SHA256 mismatch')
        patch = make_patch(source, target)
        if apply_patch(source, patch) != target:
            raise ValueError('Normalization round trip failed')
        (OUT / (digest + '.delta')).write_bytes(patch)
        print(f'{edition}: {digest[:12]}: {len(patch)} bytes')
    missing = [digest for digest in wanted if not (OUT / (digest + '.delta')).is_file()]
    if missing:
        raise ValueError(f'Missing known EXE inputs: {missing}')
    return catalog


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--inputs', type=Path, default=ROOT / 'build/gog-locales')
    parser.add_argument('--catalog-only', action='store_true')
    args = parser.parse_args()
    if args.catalog_only:
        JAVA.write_text(java_source(json.loads(CATALOG.read_text(encoding='utf-8'))), encoding='utf-8')
    else:
        prepare(args.inputs)


if __name__ == '__main__':
    main()
