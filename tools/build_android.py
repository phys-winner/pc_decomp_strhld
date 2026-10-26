"""Build the ARM64 Android stage: generated x86 C for all lifted modules plus
the Win32 HLE host, compiled with the NDK clang and linked into either a
command-line executable (adb shell bring-up) or the shared library for the APK.

Objects are reused only when the source, every header the generated code can
see, the compiler and the options are byte-identical (SHA-256 fingerprint).
"""
import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import hashlib
import json
import os
import re
from pathlib import Path
import subprocess
import sys
import time

import touch_hooks
import display_hooks
import audio_hooks
import fix_hooks
import original_hooks

ROOT = Path(__file__).resolve().parents[1]
MODULES = ("game", "mss", "bink")
EDITION = "extreme"
SDK = Path(os.environ.get("ANDROID_SDK") or os.environ.get("ANDROID_HOME") or
           os.environ.get("ANDROID_SDK_ROOT") or Path.home() / "AppData/Local/Android/Sdk")
NDK = Path(os.environ.get("ANDROID_NDK", SDK / "ndk/27.2.12479018"))
TOOLCHAIN = NDK / "toolchains/llvm/prebuilt/windows-x86_64"
CLANG = TOOLCHAIN / "bin/clang.exe"
API = 28
TARGET = f"aarch64-linux-android{API}"
OUT = ROOT / "build/arm64"
COMMON = ["--target=" + TARGET, "-fno-strict-aliasing", "-fwrapv", "-fvisibility=hidden", "-fPIC",
          "-ffp-contract=off", "-fno-math-errno", "-g0"]
GENERATED_FLAGS = COMMON + ["-O2", "-w", "-DRECOMP_GENERATED_CODE", "-DRECOMP_TRACE", "-Iandroid/runtime", "-Iandroid/host"]
HOST_FLAGS = COMMON + ["-O2", "-std=gnu11", "-Wall", "-Wno-unused-function", "-Wno-unused-variable",
                       "-DRECOMP_TRACE", "-Iandroid/runtime", "-Iandroid/host"]
NATIVE_OVERRIDES = ()

# With RECOMP_LOCAL_REGS a lifted body that uses the SSE file keeps it in a
# local copy as well (recomp_types.h: RECOMP_XMM_OUT/IN ride on the register
# transfers at every call, tail, yield and exit). The generated sources stay
# shared with the Windows build; these definitions go round each such body in
# the Android copy.
FUNCTION_START = re.compile(rb"^void sub_\w+\(void\) \{\r?$", re.M)   # generated files use CRLF
FUNCTION_END = re.compile(rb"\n\}\r?\n")
XMM_LOCAL = (b"#undef _xmm\n#undef RECOMP_XMM_OUT\n#undef RECOMP_XMM_IN\n#define _xmm l_xmm\n"
             b"#define RECOMP_XMM_OUT() memcpy(g_xmm, l_xmm, sizeof l_xmm)\n"
             b"#define RECOMP_XMM_IN() memcpy(l_xmm, g_xmm, sizeof l_xmm)\n")
XMM_GLOBAL = (b"#undef _xmm\n#undef RECOMP_XMM_OUT\n#undef RECOMP_XMM_IN\n#define _xmm g_xmm\n"
              b"#define RECOMP_XMM_OUT() ((void)0)\n#define RECOMP_XMM_IN() ((void)0)\n")


def localize_sse(text):
    parts, pos = [], 0
    for match in FUNCTION_START.finditer(text):
        end = FUNCTION_END.search(text, match.end())
        if not end or match.start() < pos:
            continue
        if b"_xmm" not in text[match.end():end.end()]:
            continue
        parts += [text[pos:match.start()], XMM_LOCAL, match.group(0),
                  b"\n    xmm_t l_xmm[8]; memcpy(l_xmm, g_xmm, sizeof l_xmm);", text[match.end():end.end()], XMM_GLOBAL]
        pos = end.end()
    return b"".join(parts + [text[pos:]]) if parts else text


# `jmp [reg*4 + table]` reaches its target through the body's `_ljump`
# switch over guest addresses; with a few hundred sparse cases clang emits a
# binary search (the script VM's opcode dispatch spent most of its time
# there). Each such jump gets a table of label addresses built from the jump
# table in the image the code was lifted from; an index past it, or an entry
# that is not a label of the body, still goes through the switch.
JUMP_TABLE = re.compile(rb"\{ _itail_tgt = \(uint32_t\)\(MEM32\((e[a-d]x|e[sd]i|ebp|esp) \* 4 \+ 0x([0-9A-F]{8})\)\); "
                        rb"goto _ljump; \}")
LABEL = re.compile(rb"^L_([0-9A-F]{8}):", re.M)
MODULE_IMAGES = {"game": "generated/input/Stronghold_Crusader_Extreme.exe",
                 "classic": "generated/input/Stronghold Crusader.exe",
                 "stronghold": "generated/input/Stronghold.exe",
                 "mss": "generated/input/Mss32.dll", "bink": "generated/input/binkw32.dll"}
# The lifted EXE module of each edition. The original Stronghold HD has none of
# Crusader's engine seams: its library excludes the Crusader-address host
# modules and links inert replacements (android/host/stronghold_original.c).
EXE_MODULE = {"extreme": "game", "classic": "classic", "stronghold": "stronghold"}
CRUSADER_ONLY_HOST = {"touch.c", "display.c", "game_ui.c"}
ORIGINAL_ONLY_HOST = {"stronghold_original.c"}


class Image:
    """A PE image's bytes by virtual address (the lifted, possibly rebased, copy)."""
    def __init__(self, path):
        data = Path(path).read_bytes()
        pe = int.from_bytes(data[0x3C:0x40], "little")
        count = int.from_bytes(data[pe + 6:pe + 8], "little")
        optional = pe + 24
        self.base = int.from_bytes(data[optional + 28:optional + 32], "little")
        table = optional + int.from_bytes(data[pe + 20:pe + 22], "little")
        self.sections = []
        for i in range(count):
            at = table + 40 * i
            vsize, va, raw_size, raw = (int.from_bytes(data[at + o:at + o + 4], "little") for o in (8, 12, 16, 20))
            self.sections.append((self.base + va, max(vsize, raw_size), raw, raw_size))
        self.data = data
        self.sha256 = hashlib.sha256(data).hexdigest()

    def u32(self, va):
        for start, size, raw, raw_size in self.sections:
            if start <= va and va + 4 <= start + size:
                offset = va - start
                return int.from_bytes(self.data[raw + offset:raw + offset + 4], "little") if offset + 4 <= raw_size else 0
        return None


_images = {}
_display_hook_count = 0
_display_hooks_seen = set()

DISPLAY_LAYOUT = {
    "classic": {
        "geometry": 0x46FF8C, "camera": 0x4E65A0, "render": 0x4E97A2,
        "clear": (0x4DAD24, 0x471430), "draw": 0x454B00, "clipped": 0x455AE0,
        "edges": [(0x431156, 0), (0x431172, 1), (0x432073, 0), (0x43208F, 1),
                  (0x4327DE, 0), (0x4327FA, 1), (0x432DC7, 0), (0x432DE3, 1),
                  (0x43A05F, 0), (0x43A0AD, 1)],
        "half": [0x4B26CF, 0x4B28CC, 0x4B2A99],
        "panel": [0x4945CE], "attention": 0x4BCFF0, "strings": 0,
        "statistics": (0x4D5F99, 0x4D6194, 0x4D6884, 0x4D701A, 0x4D70D7, 0x4D71A1, 0x4E24F3),
    },
    "game": {
        "geometry": 0x47019C, "camera": 0x4E6950, "render": 0x4E9B52,
        "clear": (0x4DB0D4, 0x471640), "draw": 0x454D40, "clipped": 0x455D20,
        "edges": [(0x4311D6, 0), (0x4311F2, 1), (0x432283, 0), (0x43229F, 1),
                  (0x4329EE, 0), (0x432A0A, 1), (0x432FD7, 0), (0x432FF3, 1),
                  (0x43A26F, 0), (0x43A2BD, 1)],
        "half": [0x4B285F, 0x4B2A5C, 0x4B2C29],
        "panel": [0x49475E, 0x431E37], "attention": 0x4BD170, "strings": 0x10,
        "statistics": (0x4D6349, 0x4D6544, 0x4D6C34, 0x4D73CA, 0x4D7487, 0x4D7551, 0x4E28A3),
    },
}
DISPLAY_EDGE_NAMES = (0x5A3374, 0x5A3404, 0x5A358C, 0x5A3574)


def display_required(module):
    layout = DISPLAY_LAYOUT[module]
    addresses = [layout[k] for k in ("geometry", "camera", "render", "attention")]
    addresses += layout["panel"] + [layout["clear"][0]] + layout["half"] + [a for a, _ in layout["edges"]] + list(layout["statistics"])
    return {f"{module}:{a:08X}" for a in addresses} | {
        f"{module}:edge{a + layout['strings']:08X}" for a in DISPLAY_EDGE_NAMES}


def custom_display_dimensions(text, module):
    """Patch proven mode-3 geometry sites, including lifted fragment copies.

    The original CPU renderers, bitmap loading and UI code remain in use.
    Each instruction/callee is asserted so a new lift cannot silently unhook it.
    """
    layout = DISPLAY_LAYOUT.get(module)
    if layout is None:
        return text, 0
    count = 0

    def record(address, matches):
        nonlocal count
        if matches:
            count += matches
            _display_hooks_seen.add(f"{module}:{address:08X}")

    def label_hook(address, code):
        nonlocal text
        label = b"L_%08X:" % address
        if label not in text:
            return
        text, matches = re.subn(rb"^" + label + rb"\r?\n", label + b"\n    " + code + b"\n", text, flags=re.M)
        record(address, matches)

    def instruction(address, expected, replacement):
        nonlocal text
        if b"/* 0x%08X:" % address not in text:
            return
        pattern = rb"^(    )([^\r\n]*?)( /\* 0x%08X: [^\r\n]*\*/)(\r?)$" % address
        def replace(match):
            if match[2] != expected:
                raise ValueError(f"{module}:{address:08X}: changed lifted instruction {match[2]!r}")
            return match[1] + replacement + match[3] + match[4]
        text, matches = re.subn(pattern, replace, text, flags=re.M)
        record(address, matches)

    label_hook(layout["geometry"], b"if (stronghold_display_dimensions(ecx)) ebx = 3;")
    label_hook(layout["camera"], b"stronghold_display_viewport(ecx);")
    label_hook(layout["render"], b"MEM32(esp + 0x18) = stronghold_display_height(MEM32(esp + 0x18));")
    address, callee = layout["clear"]
    call = b"RECOMP_CALL(sub_%08X);" % callee
    instruction(address, call, b"stronghold_display_clear(esp); " + call)
    for address, right in layout["edges"]:
        original = b"RECOMP_CALL(sub_%08X);" % layout["draw"]
        replacement = (b"{ uint32_t _hud_renderer = ecx, _hud_clip[4]; "
                       b"if (stronghold_display_hud_edge(ecx, esp, %d)) { "
                       b"stronghold_display_hud_clip(_hud_renderer, _hud_clip, 0); "
                       b"RECOMP_CALL(sub_%08X); stronghold_display_hud_clip(_hud_renderer, _hud_clip, 1); } "
                       b"else { %s } }" % (right, layout["clipped"], original))
        instruction(address, original, replacement)
    for address in layout["half"]:
        if b"L_%08X:" % address not in text:
            continue
        # Replace both the subtraction and its generated flag operand.
        pattern = (rb"(^L_%08X:\r?\n)([^\n]*(?:\n(?!L_)[^\n]*)*? /\* 0x%08X: "
                   rb"sub [^\n]*\*/)" % (address, address))
        def half_width(match):
            block = match[2]
            if block.count(b"0xF0u") != 2:
                raise ValueError(f"{module}:{address:08X}: changed mode-3 half-width subtraction")
            return match[1] + block.replace(b"0xF0u", b"stronghold_display_half_width()")
        text, matches = re.subn(pattern, half_width, text, flags=re.M)
        record(address, matches)
    for address in layout["panel"]:
        instruction(address, b"PUSH32(esp, 0x4C9u);",
                    b"PUSH32(esp, stronghold_display_width(1280) - 55);")
    instruction(layout["attention"], b"ebx = eax + 0x75;", b"ebx = stronghold_display_height(720) - 600;")
    # The original large statistics page needs 768 rows, regardless of width.
    statistics_height = 0xFD57BC if module == "classic" else 0xFD5C1C
    for address in layout["statistics"]:
        pattern = (rb"(/\* 0x%08X: cmp[^\r\n]*\*/\r?\n    _flag_a = \(uint32_t\)\()(MEM32\(0x[0-9A-F]+\))(\);)" % address)
        text, matches = re.subn(pattern, lambda m: m[1] + (b"(MEM32(0x%Xu) < 768 ? 1u : " % statistics_height) + m[2] + b")" + m[3], text)
        if b"/* 0x%08X:" % address in text and not matches:
            raise ValueError(f"{module}:{address:08X}: statistics mode comparison changed")
        record(address, matches)
    for name in DISPLAY_EDGE_NAMES:
        name += layout["strings"]
        pattern = rb"PUSH32\(esp, (0x0*%Xu)\);" % name
        text, matches = re.subn(pattern, lambda m: b"PUSH32(esp, stronghold_display_edge_name(" + m[1] + b"));", text)
        if matches:
            count += matches
            _display_hooks_seen.add(f"{module}:edge{name:08X}")
    if count:
        include = b'#include "recomp_funcs.h"'
        if include not in text:
            raise ValueError(f"{module}: generated source lacks its type header")
        declarations = (b"\nextern int stronghold_display_dimensions(uint32_t);"
                        b"\nextern void stronghold_display_viewport(uint32_t);"
                        b"\nextern uint32_t stronghold_display_width(uint32_t);"
                        b"\nextern uint32_t stronghold_display_height(uint32_t);"
                        b"\nextern uint32_t stronghold_display_half_width(void);"
                        b"\nextern uint32_t stronghold_display_edge_name(uint32_t);"
                        b"\nextern void stronghold_display_clear(uint32_t);"
                        b"\nextern int stronghold_display_hud_edge(uint32_t, uint32_t, int);"
                        b"\nextern void stronghold_display_hud_clip(uint32_t, uint32_t *, int);")
        text = text.replace(include, include + declarations, 1)
    return text, count


def module_image(module):
    """The image a generated module was lifted from, checked against its report."""
    if module not in _images:
        image = Image(ROOT / MODULE_IMAGES[module])
        report = json.loads((ROOT / f"generated/{module}-x86/report.json").read_text())
        expected = report.get("rebased_sha256") or report.get("binary_sha256")
        if expected and expected != image.sha256:
            raise SystemExit(f"{module}: image {MODULE_IMAGES[module]} does not match report.json")
        _images[module] = image
    return _images[module]


def direct_jump_tables(text, image):
    parts, pos = [], 0
    for match in FUNCTION_START.finditer(text):
        end = FUNCTION_END.search(text, match.end())
        if not end or match.start() < pos:
            continue
        body = text[match.end():end.end()]
        if b"  _ljump:" not in body or not JUMP_TABLE.search(body):
            continue
        labels = {int(label, 16) for label in LABEL.findall(body)}
        if len(labels) > 2000:
            # Bodies labelled at nearly every instruction (the CRT's memcpy
            # family: 112 tables, ~5,000 labels) took clang over ten minutes
            # with label tables; they keep the switch.
            continue
        low, high = min(labels), max(labels)

        def table_jump(jump):
            register, table = jump.group(1), int(jump.group(2), 16)
            targets = []
            while len(targets) < 1024:
                target = image.u32(table + 4 * len(targets))
                if target is None or not low <= target <= high:
                    break
                targets.append(target)
            if not targets:
                return jump.group(0)
            entries = b", ".join(b"&&L_%08X" % t if t in labels else b"&&_ljump" for t in targets)
            return (b"{ static void *const _jt[%d] = { %s }; uint32_t _jx = %s; "
                    b"_itail_tgt = (uint32_t)(MEM32(%s * 4 + 0x%08X)); if (_jx < %du) goto *_jt[_jx]; goto _ljump; }"
                    % (len(targets), entries, register, register, table, len(targets)))

        parts += [text[pos:match.end()], JUMP_TABLE.sub(table_jump, body)]
        pos = end.end()
    return b"".join(parts + [text[pos:]]) if parts else text


def override_source(source, local_xmm):
    """The file to compile for a generated source and its extra flags: a
    patched copy under build/ when it defines one of NATIVE_OVERRIDES or (with
    local_xmm) a body that uses the SSE file, and with direct jump tables."""
    original = text = source.read_bytes()
    for name in NATIVE_OVERRIDES:
        text = text.replace(b"\nvoid %s(void) {" % name.encode(), b"\nvoid %s_lifted(void) {" % name.encode(), 1)
    if source.name != "recomp_dispatch.c":
        module = source.parent.name[:-len("-x86")]
        global _display_hook_count
        text, count = custom_display_dimensions(text, module)
        _display_hook_count += count
        text, _ = touch_hooks.apply(text, module)
        text, _ = display_hooks.apply(text, module)
        text, _ = audio_hooks.apply(text, module)
        text, _ = fix_hooks.apply(text, module)
        text, _ = original_hooks.apply(text, module)
        if b"goto _ljump; }" in text:
            text = direct_jump_tables(text, module_image(module))
        if local_xmm:
            text = localize_sse(text)
    if text == original:
        return source, []
    patched = OUT / "patched" / source.parent.name / source.name
    patched.parent.mkdir(parents=True, exist_ok=True)
    if not patched.is_file() or patched.read_bytes() != text:
        patched.write_bytes(text)
    return patched, ["-I" + source.parent.relative_to(ROOT).as_posix()]


def sha_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def available_modules():
    """Exactly one executable and its shared DLL stages, all required."""
    for name in MODULES:
        if not (ROOT / f"generated/{name}-x86/report.json").exists():
            raise SystemExit(f"{name}: lift the selected edition before building")
    return MODULES


def generated_sources():
    sources = []
    modules = available_modules()
    for name in modules:
        report = json.loads((ROOT / f"generated/{name}-x86/report.json").read_text())
        files = report.get("sources")
        if not isinstance(files, list) or not files:
            raise SystemExit(f"{name}: report.json has no source list")
        sources += [ROOT / f"generated/{name}-x86" / f for f in files]
    dispatch = ROOT / f"generated/dispatch-{EDITION}/recomp_dispatch.c"
    if not dispatch.exists():
        raise SystemExit(f"{dispatch}: run tools/lift.py for the selected edition")
    sources.append(dispatch)
    return sources


def host_sources():
    excluded = {"physx.c", "crt_native.c", "sound_native.c", "hooks.c"}
    excluded |= CRUSADER_ONLY_HOST if EDITION == "stronghold" else ORIGINAL_ONLY_HOST
    return sorted(p for p in (ROOT / "android/host").glob("*.c")
                  if p.name not in excluded and not p.name.startswith(("d3d9_", "d3dx9")))


def ordered_link_objects(jobs, host_only):
    """Keep the full build's compiled-source order for host-only links too."""
    inputs = [(source, obj) for source, obj, _, _ in jobs]
    if host_only:
        generated = []
        for source in generated_sources():
            obj = OUT / "obj" / source.parent.name / (source.stem + ".o")
            if not obj.is_file():
                raise SystemExit(f"{EDITION}: --host-only requires a complete prior native build; missing {obj}")
            patched = OUT / "patched" / source.parent.name / source.name
            generated.append((patched if patched.is_file() else source, obj))
        inputs = generated + inputs
    return [str(obj) for source, obj in sorted(inputs, key=lambda item: -item[0].stat().st_size)]


def header_digest(paths):
    digest = hashlib.sha256()
    for path in sorted(paths):
        digest.update(path.relative_to(ROOT).as_posix().encode())
        digest.update(sha_file(path).encode())
    return digest.hexdigest()


def compile_one(source, obj, flags, fingerprint, cache):
    rel = source.relative_to(ROOT).as_posix()
    # Host objects differ by edition and APK/shell mode; DLL code is shared.
    cache_key = rel + "#" + obj.parent.name if source.parent.name == "host" else rel
    entry = cache.get(cache_key)
    if entry and entry.get("fingerprint") == fingerprint and obj.is_file() and sha_file(obj) == entry.get("object_sha256"):
        return cache_key, "reused", 0.0, entry
    obj.parent.mkdir(parents=True, exist_ok=True)
    start = time.time()
    command = [str(CLANG)] + flags + ["-c", str(source), "-o", str(obj)]
    result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, errors="replace")
    elapsed = time.time() - start
    if result.returncode:
        raise RuntimeError(f"{rel}: clang failed ({result.returncode})\n{result.stderr[-4000:]}")
    if result.stderr.strip():
        print(f"[warn] {rel}:\n{result.stderr.strip()[-3000:]}", flush=True)
    return cache_key, "compiled", elapsed, {"fingerprint": fingerprint, "object_sha256": sha_file(obj)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 4) - 4))
    parser.add_argument("--host-only", action="store_true", help="Recompile host sources and relink only")
    parser.add_argument("--edition", choices=("extreme", "classic", "stronghold"), default="extreme")
    parser.add_argument("--shared", action="store_true", help="Link the selected edition's APK library instead of its shell executable")
    parser.add_argument("--global-regs", action="store_true",
                        help="Keep the integer registers in globals (upstream model) instead of RECOMP_LOCAL_REGS")
    parser.add_argument("--volatile-memory", action="store_true",
                        help="Volatile guest memory accesses (upstream) instead of RECOMP_PLAIN_MEMORY")
    parser.add_argument("--pgo-generate", action="store_true",
                        help="Instrument the generated code for a profile (build/arm64-pgogen; LLVM_PROFILE_FILE "
                             "names the output, the script command 'profile' writes it)")
    parser.add_argument("--pgo-use", metavar="PROFDATA", default=None,
                        help="Optimise the generated code with a merged profile (llvm-profdata merge); "
                             "default build/pgo/void.profdata when it exists (tools/android_pgo.sh)")
    parser.add_argument("--no-pgo", action="store_true", help="Build without the profile")
    args = parser.parse_args()
    if not CLANG.is_file():
        parser.error(f"NDK compiler not found: {CLANG}; set ANDROID_NDK or ANDROID_HOME")
    # The first location gains ~12% (42-51 fps against 38-45) from the
    # profile, the map's settled phase more; a build without it is an explicit
    # choice, since it recompiles every generated object.
    if args.pgo_use is None and not args.no_pgo and not args.pgo_generate and (ROOT / "build/pgo/void.profdata").is_file():
        args.pgo_use = str(ROOT / "build/pgo/void.profdata")
    if args.no_pgo:
        args.pgo_use = None
    # The default generated code keeps registers in locals and accesses guest
    # memory plainly (first location on the Redmi Note 8 Pro: 37-43 fps against
    # 33-39 with both off). The upstream modes build in their own directories
    # (build/arm64-globalregs, build/arm64-volatilemem, ...) for A/B runs.
    global OUT, GENERATED_FLAGS, MODULES, EDITION
    EDITION = args.edition
    MODULES = (EXE_MODULE[EDITION], "mss", "bink")
    GENERATED_FLAGS = (GENERATED_FLAGS + ([] if args.global_regs else ["-DRECOMP_LOCAL_REGS"]) +
                       ([] if args.volatile_memory else ["-DRECOMP_PLAIN_MEMORY"]))
    upstream = [name for name, on in (("globalregs", args.global_regs), ("volatilemem", args.volatile_memory),
                                      ("pgogen", args.pgo_generate)) if on]
    # STRONGHOLD_BUILD_DIR gives a parallel workstream its own objects, cache and libraries.
    base = os.environ.get("STRONGHOLD_BUILD_DIR", "build/arm64")
    OUT = ROOT / base
    if upstream:
        OUT = ROOT / (base + "-" + "-".join(upstream))
    if args.pgo_generate:
        GENERATED_FLAGS = GENERATED_FLAGS + ["-fprofile-instr-generate"]
    if args.pgo_use:
        profile = Path(args.pgo_use).resolve()
        # The profile's digest joins the flags, so objects follow a new profile.
        GENERATED_FLAGS = GENERATED_FLAGS + ["-fprofile-instr-use=" + profile.as_posix(),
                                             "-DVOID_PGO_PROFILE_" + sha_file(profile)[:16]]
    os.chdir(ROOT)
    OUT.mkdir(parents=True, exist_ok=True)
    cache_path = OUT / "objects.json"
    cache = json.loads(cache_path.read_text()) if cache_path.exists() else {}
    compiler_sha = sha_file(CLANG)
    runtime_headers = [ROOT / "android/runtime/recomp_types.h"]
    # A module's sources see the runtime and their own headers; the merged
    # dispatch table (last module) sees every module's headers.
    engine_headers = [ROOT / "android/host" / name for name in ("touch.h", "touch_api.h", "display.h")]
    module_digest = {name: header_digest(runtime_headers + sorted((ROOT / f"generated/{name}-x86").glob("*.h")) +
                                       (engine_headers if name in ("game", "classic") else []))
                     for name in available_modules()}
    gen_digest = header_digest(runtime_headers + sorted(p for name in available_modules()
                                                        for p in (ROOT / f"generated/{name}-x86").glob("*.h")))
    host_digest = header_digest(runtime_headers + sorted((ROOT / "android/host").glob("*.h")) +
                                [ROOT / "reference/dlmalloc/malloc.c"])
    jobs = []
    if not args.host_only:
        _display_hooks_seen.clear()
        touch_hooks.seen.clear()
        display_hooks.seen.clear()
        audio_hooks.seen.clear()
        fix_hooks.seen.clear()
        original_hooks.seen.clear()
        for source in generated_sources():
            module = source.parent.name
            obj = OUT / "obj" / module / (source.stem + ".o")
            digest = gen_digest if source.name == "recomp_dispatch.c" else module_digest[module[:-len("-x86")]]
            source, extra = override_source(source, local_xmm=not args.global_regs)
            flags = GENERATED_FLAGS + extra
            fp = hashlib.sha256(json.dumps([sha_file(source), digest, compiler_sha, flags]).encode()).hexdigest()
            jobs.append((source, obj, flags, fp))
        module = EXE_MODULE[EDITION]
        # Crusader's custom-display geometry seams; the original Stronghold has its own mode table.
        if module in DISPLAY_LAYOUT:
            if not _display_hook_count:
                raise SystemExit(f"{EDITION}: original display geometry label was not found; refusing an unhooked build")
            missing_hooks = display_required(module) - _display_hooks_seen
            if missing_hooks:
                raise SystemExit(f"{EDITION}: missing custom-display hooks: {sorted(missing_hooks)}")
        for hooks in (touch_hooks, display_hooks, audio_hooks, fix_hooks, original_hooks):
            missing = hooks.required(module) - hooks.seen
            if missing:
                raise SystemExit(f"{EDITION}: missing {hooks.__name__}: {sorted(missing)}")
    host_flags = HOST_FLAGS + (["-DVOID_ANDROID_APP"] if args.shared else [])
    if EDITION == "classic":
        host_flags += ["-DSTRONGHOLD_CLASSIC"]
    elif EDITION == "stronghold":
        host_flags += ["-DSTRONGHOLD_ORIGINAL"]
    host_dir = ("obj/host-app" if args.shared else "obj/host") + {"classic": "-classic", "stronghold": "-stronghold"}.get(EDITION, "")
    for source in host_sources():
        obj = OUT / host_dir / (source.stem + ".o")
        fp = hashlib.sha256(json.dumps([sha_file(source), host_digest, compiler_sha, host_flags]).encode()).hexdigest()
        jobs.append((source, obj, host_flags, fp))
    # Largest first keeps the pool busy at the end.
    jobs.sort(key=lambda j: -j[0].stat().st_size)
    start = time.time()
    compiled = reused = 0
    failures = []
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futures = [pool.submit(compile_one, s, o, f, fp, cache) for s, o, f, fp in jobs]
        for future in as_completed(futures):
            try:
                rel, status, elapsed, entry = future.result()
            except RuntimeError as error:
                failures.append(str(error))
                print(str(error), flush=True)
                continue
            cache[rel] = entry
            if status == "compiled":
                compiled += 1
                if elapsed > 20:
                    print(f"[slow] {rel}: {elapsed:.1f}s", flush=True)
            else:
                reused += 1
            if (compiled + reused) % 50 == 0:
                print(f"[progress] {compiled + reused}/{len(jobs)} ({time.time() - start:.0f}s)", flush=True)
                cache_path.write_text(json.dumps(cache, indent=1))
    cache_path.write_text(json.dumps(cache, indent=1))
    print(f"compiled {compiled}, reused {reused}, failed {len(failures)} in {time.time() - start:.0f}s", flush=True)
    if failures:
        sys.exit(1)
    objects = ordered_link_objects(jobs, args.host_only)
    response = OUT / "link.rsp"
    response.write_text("\n".join(o.replace("\\", "/") for o in objects))
    output = OUT / ({"classic": "libstronghold_classic.so", "stronghold": "libstronghold_sh.so"}.get(EDITION, "libstronghold.so")
                    if args.shared else
                    {"classic": "stronghold-classic-arm64", "stronghold": "stronghold-sh-arm64"}.get(EDITION, "stronghold-arm64"))
    link = [str(CLANG), "--target=" + TARGET, "-fuse-ld=lld", "-Wl,--gc-sections", "-Wl,-z,nostart-stop-gc", "-Wl,-z,max-page-size=16384", "-Wl,-z,common-page-size=16384",
            "-Wl,--build-id=sha1", "@" + str(response), "-o", str(output), "-lm", "-llog", "-ldl", "-lEGL", "-lGLESv3"]
    link += ["-shared", "-landroid", "-lOpenSLES", "-laaudio", "-ljnigraphics"] if args.shared else ["-pie", "-laaudio", "-landroid"]
    if args.pgo_generate:
        link.append("-fprofile-instr-generate")
    start = time.time()
    result = subprocess.run(link, cwd=ROOT, capture_output=True, text=True, errors="replace")
    if result.returncode:
        print(result.stderr[-6000:])
        sys.exit(1)
    print(f"linked {output} ({output.stat().st_size / 1e6:.1f} MB) in {time.time() - start:.0f}s")


if __name__ == "__main__":
    main()
