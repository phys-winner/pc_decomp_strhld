"""Strip packaging copies and store game libraries as ARM64-filtered XZ chunks.

Build libraries remain intact. The final APK is decoded and checked again after
signing; a successful strip alone does not prove ELF loader compatibility.
"""
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import zipfile

from verify_elf_pages import inspect

ROOT = Path(__file__).resolve().parents[1]
GAMES = ('libstronghold.so', 'libstronghold_classic.so', 'libstronghold_sh.so')
UNPACKER = 'libstronghold_xz.so'
CHUNK = 16 << 20


def sha(path):
    with Path(path).open('rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()


def xz_tool():
    for candidate in (os.environ.get('XZ'), shutil.which('xz'), r'C:\Program Files\Git\mingw64\bin\xz.exe'):
        if candidate and Path(candidate).is_file():
            version = subprocess.check_output([candidate, '--robot', '--version'], text=True)
            number = int(next(line.split('=')[1] for line in version.splitlines() if line.startswith('XZ_VERSION=')))
            if number >= 50040002:
                return str(candidate)
    raise RuntimeError('XZ Utils >= 5.4 required for ARM64 compression; set XZ or install Git for Windows')


def sections(data):
    offset = struct.unpack_from('<Q', data, 40)[0]
    stride, count, names = struct.unpack_from('<HHH', data, 58)
    rows = [struct.unpack_from('<IIQQQQIIQQ', data, offset + i * stride) for i in range(count)]
    table = rows[names]
    strings = data[table[4]:table[4] + table[5]]
    return [strings[row[0]:].split(b'\0', 1)[0].decode('ascii') for row in rows]


def verify_stripped(source, target, llvm):
    before, after = Path(source).read_bytes(), Path(target).read_bytes()
    original, stripped = inspect(before), inspect(after)
    names = sections(after)
    if '.symtab' in names or any(name.startswith(('.debug', '.zdebug')) for name in names):
        raise ValueError('Static symbols/debug sections remain: ' + str(target))
    if original['pt_load'] != stripped['pt_load']:
        raise ValueError('Strip changed PT_LOAD layout: ' + str(target))
    # Only section-header metadata in the ELF header may change inside a LOAD.
    normalized = bytearray(before)
    normalized[40:48] = after[40:48]
    normalized[58:64] = after[58:64]
    for row in original['pt_load']:
        at, size = row['offset'], row['filesz']
        if normalized[at:at + size] != after[at:at + size]:
            raise ValueError('Strip changed loaded bytes: ' + str(target))

    def exports(path):
        output = subprocess.check_output([str(llvm / 'llvm-nm.exe'), '-D', '--defined-only', str(path)], text=True)
        return sorted(tuple(line.split()) for line in output.splitlines() if line.strip())
    if exports(source) != exports(target):
        raise ValueError('Strip changed dynamic exports: ' + str(target))
    return dict(source_bytes=len(before), bytes=len(after), source_sha256=sha(source), sha256=sha(target),
                exports_unchanged=True, loaded_bytes_unchanged=True, stripped=True, elf_16kb=stripped)


def compress_library(source, cache):
    digest = sha(source)
    cache.mkdir(parents=True, exist_ok=True)
    output = cache / f'{digest}-{CHUNK >> 20}m-arm64-lp2.xz'
    manifest = output.with_suffix('.json')
    if output.is_file() and manifest.is_file():
        previous = json.loads(manifest.read_text())
        if previous['sha256'] == digest and sha(output) == previous['xz_sha256']:
            return output, previous
    command = [xz_tool(), '--stdout', '--arm64', f'--lzma2=preset=9e,dict={CHUNK},lc=0,lp=2,pb=2',
               '--check=crc32', '--threads=1']

    def pack(offset):
        with source.open('rb') as file:
            file.seek(offset)
            data = file.read(CHUNK)
        return subprocess.run(command, input=data, capture_output=True, check=True).stdout

    with ThreadPoolExecutor(max_workers=2) as pool:
        chunks = list(pool.map(pack, range(0, source.stat().st_size, CHUNK)))
    part = output.with_suffix('.part')
    with part.open('wb') as file:
        for chunk in chunks:
            file.write(chunk)
    part.replace(output)
    report = dict(bytes=source.stat().st_size, sha256=digest, chunk_bytes=CHUNK,
                  packed_sizes=[len(chunk) for chunk in chunks], xz_bytes=output.stat().st_size,
                  xz_sha256=sha(output), command=command)
    manifest.write_text(json.dumps(report, indent=2) + '\n')
    return output, report


def verify_chunk_table(path, compression):
    listing = subprocess.check_output([xz_tool(), '--robot', '--list', '--verbose', str(path)], text=True)
    streams = [line.split('\t') for line in listing.splitlines() if line.startswith('stream\t')]
    if len(streams) != len(compression['packed_sizes']):
        raise ValueError('XZ stream count differs from chunk table')
    packed_at = plain_at = 0
    for i, (row, size) in enumerate(zip(streams, compression['packed_sizes'])):
        plain = min(compression['chunk_bytes'], compression['bytes'] - plain_at)
        expected = [i + 1, 1, packed_at, plain_at, size, plain]
        if list(map(int, row[1:7])) != expected or row[8:] != ['CRC32', '0']:
            raise ValueError('XZ stream boundaries differ from chunk table')
        packed_at += size
        plain_at += plain
    if plain_at != compression['bytes'] or packed_at != Path(path).stat().st_size:
        raise ValueError('XZ chunk totals differ')


def prepare(libraries, llvm, work):
    work.mkdir(parents=True, exist_ok=True)
    expected = set(GAMES) | {'libinnoextract.so', UNPACKER}
    if {source.name for source in libraries} != expected or len(libraries) != len(expected):
        raise ValueError('Game APK requires three games, GOG reader and XZ decoder')
    entries, report, index = {}, {}, []
    for source in libraries:
        target = work / source.name
        subprocess.run([str(llvm / 'llvm-strip.exe'), '--strip-unneeded', '-o', str(target), str(source)], check=True)
        row = verify_stripped(source, target, llvm)
        report[source.name] = row
        if source.name in GAMES:
            packed, compression = compress_library(target, ROOT / 'build/native-xz')
            row['xz'] = compression
            entries['assets/native/' + source.name + '.xz'] = (packed, zipfile.ZIP_STORED)
            index.append(f'{source.name} {row["bytes"]} {row["sha256"]} {CHUNK} '
                         + ','.join(map(str, compression['packed_sizes'])))
            print(f'{source.name}: {row["source_bytes"]} -> stripped {row["bytes"]} -> XZ {compression["xz_bytes"]}', flush=True)
        else:
            entries['lib/arm64-v8a/' + source.name] = (target, zipfile.ZIP_DEFLATED)
    path = work / 'index.txt'
    path.write_text('\n'.join(index) + '\n', encoding='ascii', newline='\n')
    entries['assets/native/index.txt'] = (path, zipfile.ZIP_STORED)
    return entries, report


def verify_payload(archive, report, work):
    """Check final signed APK bytes, reconstruct every game, and audit the actual ELFs."""
    work.mkdir(parents=True, exist_ok=True)
    names = archive.namelist()
    if len(names) != len(set(names)):
        raise ValueError('Duplicate APK entries')
    expected = {'lib/arm64-v8a/' + name for name in ('libinnoextract.so', UNPACKER)}
    if {name for name in names if name.startswith('lib/')} != expected:
        raise ValueError('Unexpected native APK entries')
    expected_index = []
    for name in GAMES:
        row = report[name]
        compression = row['xz']
        expected_index.append(f'{name} {row["bytes"]} {row["sha256"]} {CHUNK} '
                              + ','.join(map(str, compression['packed_sizes'])))
        entry = 'assets/native/' + name + '.xz'
        packed = work / (name + '.xz')
        if archive.getinfo(entry).compress_type != zipfile.ZIP_STORED:
            raise ValueError('XZ payload must not be ZIP-compressed')
        with archive.open(entry) as source, packed.open('wb') as target:
            shutil.copyfileobj(source, target)
        if sha(packed) != compression['xz_sha256'] or packed.stat().st_size != sum(compression['packed_sizes']):
            raise ValueError('APK XZ differs: ' + name)
        verify_chunk_table(packed, compression)
        with (work / name).open('wb') as target:
            subprocess.run([xz_tool(), '--decompress', '--stdout', str(packed)], stdout=target, check=True)
    if archive.read('assets/native/index.txt').decode('ascii') != '\n'.join(expected_index) + '\n':
        raise ValueError('APK native index differs')
    for name in ('libinnoextract.so', UNPACKER):
        with archive.open('lib/arm64-v8a/' + name) as source, (work / name).open('wb') as target:
            shutil.copyfileobj(source, target)
    for name, row in report.items():
        target = work / name
        if target.stat().st_size != row['bytes'] or sha(target) != row['sha256']:
            raise ValueError('Decoded/packaged library differs: ' + name)
        data = target.read_bytes()
        inspect(data)
        if any(section == '.symtab' or section.startswith(('.debug', '.zdebug')) for section in sections(data)):
            raise ValueError('Unstripped final library: ' + name)
    return dict(status='PASS', libraries=len(report), xz_roundtrip=True, elf_16kb=True)
