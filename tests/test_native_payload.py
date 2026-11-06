"""Focused checks for the stripped XZ package and the production Java cache loader."""
import ctypes
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import android_native_payload as payload
import build_android


class NativePayloadTests(unittest.TestCase):
    def test_strip_preserves_exports_and_loaded_bytes_and_rejects_changes(self):
        llvm = build_android.TOOLCHAIN / 'bin'
        if not (llvm / 'llvm-strip.exe').is_file() or not (ROOT / 'build/arm64/libstronghold.so').is_file():
            self.skipTest('NDK and completed native build required')
        with tempfile.TemporaryDirectory() as directory:
            for name in payload.GAMES:
                source, target = ROOT / 'build/arm64' / name, Path(directory) / name
                subprocess.run([str(llvm / 'llvm-strip.exe'), '--strip-unneeded', '-o', str(target), str(source)], check=True)
                result = payload.verify_stripped(source, target, llvm)
                self.assertTrue(result['exports_unchanged'] and result['loaded_bytes_unchanged'])
                data = bytearray(target.read_bytes())
                executable = next(row for row in result['elf_16kb']['pt_load'] if row['flags'] & 1)
                data[executable['offset'] + 4] ^= 1
                target.write_bytes(data)
                with self.assertRaisesRegex(ValueError, 'changed loaded bytes'):
                    payload.verify_stripped(source, target, llvm)

    def test_chunk_streams_decode_with_runtime_memory_limit(self):
        tool = Path(payload.xz_tool())
        dll = tool.with_name('liblzma-5.dll')
        if not dll.is_file():
            self.skipTest('Git for Windows liblzma required for the native API check')
        library = ctypes.CDLL(str(dll))
        decode = library.lzma_stream_buffer_decode
        decode.argtypes = [ctypes.POINTER(ctypes.c_uint64), ctypes.c_uint32, ctypes.c_void_p,
                           ctypes.c_void_p, ctypes.POINTER(ctypes.c_size_t), ctypes.c_size_t,
                           ctypes.c_void_p, ctypes.POINTER(ctypes.c_size_t), ctypes.c_size_t]
        decode.restype = ctypes.c_int
        data = bytes(range(256)) * 4096
        packed = subprocess.run([str(tool), '--stdout', '--arm64', '--lzma2=dict=16MiB,lc=0,lp=2,pb=2',
                                 '--check=crc32'], input=data, capture_output=True, check=True).stdout
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / 'fixture.xz'
            source.write_bytes(packed + packed)
            table = dict(bytes=len(data) * 2, chunk_bytes=len(data), packed_sizes=[len(packed), len(packed)])
            payload.verify_chunk_table(source, table)
            table['packed_sizes'] = [len(packed) - 4, len(packed) + 4]
            with self.assertRaisesRegex(ValueError, 'boundaries differ'):
                payload.verify_chunk_table(source, table)

        def run(encoded, output_size):
            memory = ctypes.c_uint64(40 << 20)
            before, after = ctypes.c_size_t(), ctypes.c_size_t()
            output = ctypes.create_string_buffer(output_size)
            status = decode(ctypes.byref(memory), 0, None, encoded, ctypes.byref(before), len(encoded),
                            output, ctypes.byref(after), output_size)
            return status, before.value, after.value, output.raw
        self.assertEqual(run(packed, len(data)), (0, len(packed), len(data), data))
        self.assertNotEqual(run(packed[:-1], len(data))[0], 0)
        corrupt = bytearray(packed); corrupt[len(corrupt) // 2] ^= 0x80
        self.assertNotEqual(run(bytes(corrupt), len(data))[0], 0)
        self.assertNotEqual(run(packed, len(data) - 1)[0], 0)
        status, consumed, _, _ = run(packed + packed, len(data))
        self.assertEqual(status, 0)
        self.assertNotEqual(consumed, len(packed) * 2, 'Runtime must reject trailing streams using its consumed-byte check')

    def test_java_cache_loader(self):
        jdk = Path(os.environ.get('JAVA_HOME', 'C:/Portable/jdk-24.0.1')) / 'bin'
        if not (jdk / 'javac.exe').is_file():
            self.skipTest('JDK required')
        stubs = {
            'android/content/Context.java': '''package android.content;
                import java.io.File; import android.content.res.AssetManager;
                public class Context { private final File files; private final AssetManager assets;
                public Context(File assets, File files) { this.files=files; this.assets=new AssetManager(assets); }
                public File getFilesDir() { return files; } public AssetManager getAssets() { return assets; } }''',
            'android/content/res/AssetManager.java': '''package android.content.res;
                import java.io.*; public class AssetManager { private final File root;
                public AssetManager(File root) { this.root=root; }
                public InputStream open(String name) throws IOException { return new FileInputStream(new File(root,name)); } }''',
            'android/util/Log.java': 'package android.util; public class Log { public static int i(String t,String m) { return 0; } }',
            'android/os/SystemClock.java': 'package android.os; public class SystemClock { public static long elapsedRealtime() { return System.nanoTime()/1000000; } }',
            'org/stronghold/port/GameFiles.java': '''package org.stronghold.port;
                import java.io.*; import java.nio.file.*; import java.security.*;
                final class GameFiles { static String sha256(File file) throws IOException {
                try { byte[] hash=MessageDigest.getInstance("SHA-256").digest(Files.readAllBytes(file.toPath()));
                StringBuilder text=new StringBuilder(); for(byte b:hash) text.append(String.format("%02x",b&255));
                return text.toString(); } catch(NoSuchAlgorithmException e) { throw new AssertionError(e); } } }''',
        }
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            sources = []
            for name, text in stubs.items():
                source = work / name; source.parent.mkdir(parents=True, exist_ok=True); source.write_text(text)
                sources.append(source)
            sources += [ROOT / 'android/app/src/org/stronghold/port/NativeLibrary.java', ROOT / 'tests/NativeLibrarySelfcheck.java']
            subprocess.run([str(jdk / 'javac.exe'), '--release', '8', '-d', str(work / 'classes'), *map(str, sources)], check=True)
            subprocess.run([str(jdk / 'java.exe'), '-ea', '-cp', str(work / 'classes'),
                            'org.stronghold.port.NativeLibrarySelfcheck', str(work / 'fixture')], check=True)


if __name__ == '__main__':
    unittest.main()
