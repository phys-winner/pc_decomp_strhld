"""Check the three AOT dispatches and edition constants against the original PEs."""
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess

import pefile
from build_android import CLANG, custom_display_dimensions, display_required, _display_hooks_seen
import original_hooks

ROOT = Path(__file__).resolve().parents[1]


def edition_constants(define):
    """GAME_* integers of edition.h as the host compiler sees them for one edition."""
    command = [str(CLANG), "-E", "-dM", "-x", "c"] + ([f"-D{define}"] if define else []) + [str(ROOT / "android/host/edition.h")]
    text = subprocess.run(command, capture_output=True, text=True, check=True).stdout
    return {key: int(value, 16) for key, value in
            re.findall(r"#define (GAME_ENTRY|GAME_SCREEN|GAME_MODE_CALC) (0x[0-9A-F]+)u", text)}


def check_dispatch(edition, module, others):
    expected = set()
    for stage in (module, "mss", "bink"):
        text = (ROOT / f"generated/{stage}-x86/recomp_funcs.h").read_text()
        entries = {int(address, 16) for address in re.findall(r"void sub_([0-9A-F]+)\(void\);", text)}
        assert not expected.intersection(entries), f"{edition}: overlapping live guest addresses"
        expected.update(entries)
    dispatch = (ROOT / f"generated/dispatch-{edition}/recomp_dispatch.c").read_text()
    actual = [int(address, 16) for address in re.findall(r"\{0x([0-9A-F]+)u,", dispatch)]
    assert len(actual) == len(set(actual)) and set(actual) == expected
    assert f'"../{module}-x86/recomp_funcs.h"' in dispatch
    for other in others:
        assert f'"../{other}-x86/recomp_funcs.h"' not in dispatch
    return actual


def main():
    inputs = json.loads((ROOT / "config/inputs.json").read_text())
    images = {}
    for edition, module, name in (("extreme", "game", "Stronghold_Crusader_Extreme.exe"),
                                  ("classic", "classic", "Stronghold Crusader.exe")):
        raw = (ROOT / "generated/input" / name).read_bytes()
        assert hashlib.sha256(raw).hexdigest() == inputs[name]
        pe = pefile.PE(data=raw)
        images[edition] = pe
        constants = edition_constants("STRONGHOLD_CLASSIC" if edition == "classic" else None)
        assert constants["GAME_ENTRY"] == pe.OPTIONAL_HEADER.ImageBase + pe.OPTIONAL_HEADER.AddressOfEntryPoint
        # The original constructor call proves the selected screen's address.
        ctor = 0x467BE0 if edition == "classic" else 0x467DE0
        mov_ecx = b"\xB9" + struct.pack("<I", constants["GAME_SCREEN"])
        assert any(pe.get_data(match.start() + 5, 1) == b"\xE8" and
                   (pe.OPTIONAL_HEADER.ImageBase + match.start() + 10 +
                    struct.unpack("<i", pe.get_data(match.start() + 6, 4))[0] == ctor)
                   for match in re.finditer(re.escape(mov_ecx), pe.get_memory_mapped_image()))
        actual = check_dispatch(edition, module, ("game" if module == "classic" else "classic", "stronghold"))
        hook_count = 0
        _display_hooks_seen.clear()
        report = json.loads((ROOT / f"generated/{module}-x86/report.json").read_text())
        for name in report["sources"]:
            source = (ROOT / f"generated/{module}-x86" / name).read_bytes()
            patched, count = custom_display_dimensions(source, module)
            hook_count += count
            assert count >= patched.count(b"if (stronghold_display_dimensions(ecx)) ebx = 3;")
            assert custom_display_dimensions(source, "mss") == (source, 0)
        assert hook_count > 0
        assert not (display_required(module) - _display_hooks_seen), sorted(display_required(module) - _display_hooks_seen)
        print(f"{edition}: original hash/entry/screen constructor; {len(actual)} isolated dispatch entries: PASS")
    extreme = images["extreme"].get_data(0x470030 - 0x400000, 24)
    classic = images["classic"].get_data(0x46FE20 - 0x400000, 24)
    assert extreme == classic
    print("edition mode-calculation prologues match original PEs: PASS")
    # The original Stronghold HD: no Crusader seams; its own mode and frame seams.
    raw = (ROOT / "generated/input/Stronghold.exe").read_bytes()
    assert hashlib.sha256(raw).hexdigest() == inputs["Stronghold.exe"]
    pe = pefile.PE(data=raw)
    constants = edition_constants("STRONGHOLD_ORIGINAL")
    assert constants["GAME_ENTRY"] == pe.OPTIONAL_HEADER.ImageBase + pe.OPTIONAL_HEADER.AddressOfEntryPoint
    assert constants["GAME_SCREEN"] == 0 and constants["GAME_MODE_CALC"] == 0, "Crusader's mode-3 override must stay off"
    # 532AAD mov edx, [0B18324h]; 532AB5 mov ecx, 0B182C8h (the saved mode for 461540).
    assert pe.get_data(0x532AAD - 0x400000, 6) == bytes([0x8B, 0x15]) + struct.pack("<I", 0xB18324)
    assert pe.get_data(0x532AB5 - 0x400000, 5) == bytes([0xB9]) + struct.pack("<I", 0xB182C8)
    # 532EBA mov ecx, 0BCAF70h before the per-frame scroll update 4599E0.
    assert pe.get_data(0x532EBA - 0x400000, 5) == bytes([0xB9]) + struct.pack("<I", 0xBCAF70)
    actual = check_dispatch("stronghold", "stronghold", ("game", "classic"))
    original_hooks.seen.clear()
    report = json.loads((ROOT / "generated/stronghold-x86/report.json").read_text())
    for name in report["sources"]:
        source = (ROOT / "generated/stronghold-x86" / name).read_bytes()
        assert custom_display_dimensions(source, "stronghold") == (source, 0)
        original_hooks.apply(source, "stronghold")
    assert not (original_hooks.required("stronghold") - original_hooks.seen)
    print(f"stronghold: original hash/entry, mode and frame seams; {len(actual)} isolated dispatch entries: PASS")


if __name__ == "__main__":
    main()
