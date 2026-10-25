"""Keep only the explicit Quit voice across menu transition; drain it at shutdown."""
import re

seen = set()
LAYOUT = {
    "game": ((0x57D60F, 0x47B400, 0x1165678, 0),
             (0x495705, 0x47B750, 0x1165678, 0x1163EA8),
             (0x495493, 0x47B750, 0x1165678, -1),
             (0x48CD64, 0x47B750, 0x1165678, -1)),
    "classic": ((0x57D1BF, 0x47B210, 0x1165218, 0),
                (0x495575, 0x47B560, 0x1165218, 0x1163A48),
                (0x495303, 0x47B560, 0x1165218, -1),
                (0x48CC34, 0x47B560, 0x1165218, -1)),
}
VOICE = {"game": (0x44A7E0, 0x47BFA8, 0x47A710, 0x1165678),
         "classic": (0x44A5A0, 0x47BDB8, 0x47A520, 0x1165218)}
PC = re.compile(rb"/\* 0x([0-9A-F]{8}):")


def required(module):
    if module not in LAYOUT:
        return set()
    return ({f"{module}:{site:08X}" for site, *_ in LAYOUT[module]} |
            {f"{module}:{site:08X}" for site in VOICE[module][:3]})


def apply(text, module):
    if module not in LAYOUT:
        return text, 0
    total = 0
    for address, callee, _, action in LAYOUT[module]:
        expected = text.count(b"/* 0x%08X:" % address)
        pattern = (rb"^(    )(RECOMP_CALL\(sub_%08X\);)( /\* 0x%08X:[^\r\n]*\*/)(\r?)$"
                   % (callee, address))
        guard = b"if (MEM32(0x%08Xu) == 7u) " % action if action > 0 else b""
        helper = b"stronghold_audio_finish_speech" if action == 0 else b"stronghold_audio_menu_handoff"
        def replace(match):
            return (match[1] + b"RECOMP_REGS_OUT(); " + guard + helper +
                    b"(ecx); RECOMP_REGS_IN(); " + match[2] + match[3] + match[4])
        text, count = re.subn(pattern, replace, text, flags=re.M)
        if count != expected:
            raise ValueError(f"{module}: audio seam {address:08X} has {expected} instructions but {count} valid calls")
        if count:
            seen.add(f"{module}:{address:08X}")
        total += count
    request, started, stop, manager = VOICE[module]
    output = []
    for line in text.splitlines(keepends=True):
        match = PC.search(line)
        pc = int(match[1], 16) if match else None
        if pc == request:
            output.append(("    RECOMP_REGS_OUT(); if (MEM32(esp+4) == 1u) stronghold_audio_quit_request(0x%Xu); RECOMP_REGS_IN();\n" % manager).encode())
        elif pc == stop:
            output.append(b"    { RECOMP_REGS_OUT(); int _keep_exit_voice = stronghold_audio_keep_stream(ecx, MEM32(esp+4), MEM32(esp)); RECOMP_REGS_IN(); if (_keep_exit_voice) { esp+=8; RECOMP_FLAGS_OUT(); RECOMP_REGS_OUT(); return; } }\n")
        output.append(line)
        if pc == started:
            if b"RECOMP_ICALL(MEM32(0x59F2B8));" not in line:
                raise ValueError(f"{module}: voice-start seam is not the original AIL_start_stream import")
            output.append(b"    RECOMP_REGS_OUT(); stronghold_audio_stream_started(esi, edi, MEM32(esp+0x14)); RECOMP_REGS_IN();\n")
        if pc in (request, started, stop):
            seen.add(f"{module}:{pc:08X}")
            total += 1
    text = b"".join(output)
    if total:
        text = text.replace(b'#include "recomp_funcs.h"', b'#include "recomp_funcs.h"\n'
            b'extern void stronghold_audio_finish_speech(uint32_t);\n'
            b'extern void stronghold_audio_menu_handoff(uint32_t);\n'
            b'extern void stronghold_audio_quit_request(uint32_t);\n'
            b'extern void stronghold_audio_stream_started(uint32_t,uint32_t,uint32_t);\n'
            b'extern int stronghold_audio_keep_stream(uint32_t,uint32_t,uint32_t);', 1)
    return text, total


if __name__ == "__main__":
    import hashlib
    import json
    from pathlib import Path
    import struct
    import pefile

    root = Path(__file__).resolve().parents[1]
    hashes = json.loads((root / "config/inputs.json").read_text())
    results = []
    for module, seams in LAYOUT.items():
        name = "Stronghold_Crusader_Extreme.exe" if module == "game" else "Stronghold Crusader.exe"
        raw = (root / "generated/input" / name).read_bytes()
        digest = hashlib.sha256(raw).hexdigest()
        assert digest == hashes[name]
        image = pefile.PE(data=raw).get_memory_mapped_image()
        for site, callee, manager, _ in seams:
            at = site - 0x400000
            assert image[at - 5:at] == b"\xB9" + struct.pack("<I", manager)
            assert image[at:at + 5] == b"\xE8" + struct.pack("<i", callee - site - 5)
        request, started, stop, _ = VOICE[module]
        assert image[request-0x400000:request-0x400000+3] == b"\x83\xEC\x44"
        assert image[started-0x400000:started-0x400000+6] == b"\xFF\x15\xB8\xF2\x59\x00"
        assert image[stop-0x400000:stop-0x400000+4] == b"\x53\x56\x8B\xF1"
        seen.clear()
        copies = 0
        call_copies = 0
        for source in (root / f"generated/{module}-x86").glob("recomp_*.c"):
            original = source.read_bytes()
            patched, count = apply(original, module)
            copies += count
            assert patched.count(b"stronghold_audio_finish_speech(ecx)") + patched.count(b"stronghold_audio_menu_handoff(ecx)") <= count
            # Other callers, including the demo purchase screen, are untouched.
            removed = 0
            checked_callees = set()
            for site, callee, _, action in seams:
                if callee in checked_callees:
                    continue
                checked_callees.add(callee)
                old_calls = re.findall(rb"^    RECOMP_CALL\(sub_%08X\);.*$" % callee, original, re.M)
                new_calls = re.findall(rb"^    RECOMP_CALL\(sub_%08X\);.*$" % callee, patched, re.M)
                removed += len(old_calls) - len(new_calls)
                if action > 0 and b"/* 0x%08X:" % site in original:
                    assert b"if (MEM32(0x%08Xu) == 7u) stronghold_audio_menu_handoff(ecx);" % action in patched
            call_copies += removed
            assert removed == patched.count(b"stronghold_audio_finish_speech(ecx)") + patched.count(b"stronghold_audio_menu_handoff(ecx)")
        assert copies and not (required(module) - seen)
        results.append(dict(module=module, original_sha256=digest,
                            seams=[dict(site=hex(s), callee=hex(c), manager=hex(m),
                                        only_action7_at=hex(a) if a > 0 else None) for s,c,m,a in seams],
                            lifted_copies=copies, call_copies=call_copies, voice_anchors=[hex(x) for x in VOICE[module][:3]], status="PASS"))
        print(f"{module}: hash-pinned selective shutdown/nonblocking menu, {copies} lifted copies: PASS")
    (root / "generated/audio-hook-proof.json").write_text(json.dumps(results, indent=2) + "\n")
