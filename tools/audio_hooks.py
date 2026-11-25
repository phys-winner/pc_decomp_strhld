"""Bind Miles output to Android and preserve the explicit Quit voice."""
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
MILES_OUTPUT = (0x211119D0, 0x21111A50, 0x21111E70, 0x21111EB0, 0x21112330, 0x21114EC0)
MILES_FILL = 0x21112AF5
MILES_RESET_TAIL = 0x21112D9C
MILES_COPY_LOOPS = {
    0x2113D238: (0x2113D254, 4, True),
    0x2113D262: (0x2113D288, 4, False),
    0x2113D299: (0x2113D2B8, 1, True),
    0x2113D2D4: (0x2113D2F5, 1, False),
}
BINK_RGB_LOOPS = ((0x30025320, 0x30025472, 0, False), (0x30025490, 0x300255E1, 1, False),
                  (0x30025670, 0x300257E1, 0, True), (0x30025840, 0x300259B0, 1, True))
MILES_MERGE_LOOPS = (
    0x21139000, 0x21139036, 0x21139079, 0x211390A4,
    0x211390D7, 0x2113913C, 0x211391A0, 0x211391DB,
    0x2113921F, 0x2113926A, 0x211392D6, 0x21139308,
    0x21139354, 0x211393D7, 0x21139458, 0x211394A6,
)
MILES_RESAMPLE_LOOPS = (
    0x211394F7, 0x21139555, 0x211395B8, 0x2113960F,
    0x211399B5, 0x21139A37, 0x21139ABB, 0x21139B16,
    0x21139FC5, 0x2113A029, 0x2113A0B5, 0x2113A113,
    0x2113A51E, 0x2113A5C2, 0x2113A663, 0x2113A6D1,
)
MILES_UPSAMPLE_LOOPS = (
    0x2113BCA0, 0x2113BD20, 0x2113BDA0, 0x2113BE10,
    0x2113BEA0, 0x2113BF50, 0x2113C020, 0x2113C0B0,
    0x2113C170, 0x2113C200, 0x2113C280, 0x2113C300,
    0x2113C3C0, 0x2113C4C0, 0x2113C5B0, 0x2113C680,
)


def required(module):
    if module == "bink":
        return {f"bink:{site:08X}" for site, *_ in BINK_RGB_LOOPS}
    if module == "mss":
        sites = (*MILES_OUTPUT, MILES_FILL, MILES_RESET_TAIL, *MILES_COPY_LOOPS,
                 *MILES_MERGE_LOOPS, *MILES_RESAMPLE_LOOPS, *MILES_UPSAMPLE_LOOPS)
        return {f"mss:{site:08X}" for site in sites}
    if module not in LAYOUT:
        return set()
    return ({f"{module}:{site:08X}" for site, *_ in LAYOUT[module]} |
            {f"{module}:{site:08X}" for site in VOICE[module][:3]})


def apply(text, module):
    if module == "bink":
        return bink_rgb16(text)
    if module == "mss":
        total = 0
        for site in MILES_OUTPUT:
            name = b"\nvoid sub_%08X" % site
            entry = name + b"(void) {"
            if entry in text:
                text = text.replace(entry, name + b"_lifted(void) {", 1)
                seen.add(f"mss:{site:08X}")
                total += 1
        # Keep Miles' mix/background guards and shared timer epilogue.
        anchor = rb"(?m)^(L_21112AF5:\r?\n)(    RECOMP_CALL\(sub_211118C0\); /\* 0x21112AF5: call 0x211118c0 \*/)"
        hook = rb"\1    RECOMP_FLAGS_OUT(); RECOMP_REGS_OUT(); stronghold_audio_miles_fill(esi); RECOMP_REGS_IN(); goto L_21112C55;\n\2"
        text, count = re.subn(anchor, hook, text)
        if count:
            text = b"#include <stdint.h>\nextern void stronghold_audio_miles_fill(uint32_t driver);\n" + text
            seen.add(f"mss:{MILES_FILL:08X}")
            total += count
        tail = b"    goto L_21111EB0; /* 0x21112D9C: jmp 0x21111eb0 */"
        count = text.count(tail)
        if count:
            text = text.replace(tail, b"    RECOMP_FLAGS_OUT(); RECOMP_REGS_OUT(); sub_21111EB0(); return; "
                                      b"/* 0x21112D9C: jmp 0x21111eb0 */")
            seen.add(f"mss:{MILES_RESET_TAIL:08X}")
        text, copies = mix_copy(text)
        text, merges = mix_merge(text)
        text, samples = mix_resample(text)
        text, linear = mix_upsample(text)
        return text, total + count + copies + merges + samples + linear
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


def mix_copy(text):
    copies = 0
    for pc, (edge, samples, wide) in MILES_COPY_LOOPS.items():
        # Leave the last iteration to the lifted loop to publish x86 flags and MMX scratch.
        label = rb"(?m)^(L_%08X:\r?\n)(?=[^\n]*0x%08X:)" % (pc, pc)
        bound = b"eax" if samples == 4 else b"(ecx > esi ? (uint32_t)(((uint64_t)ecx - esi + 3) / 4) : 0)"
        hook = (b"    { uint32_t _copy = " + bound + b";\n"
                b"      if (_copy > 1 && _copy <= 4097 && (!recomp_yield_hook || "
                b"(uint64_t)g_backedges + _copy - 1 < RECOMP_YIELD_EVERY)) {\n"
                b"        uint32_t _bulk = (_copy - 1) * %du;\n"
                b"        if (stronghold_miles_copy_bulk(esi, edi, _bulk, %d)) {\n"
                b"          esi += _bulk * 4; edi += _bulk * %du; g_backedges += _copy - 1; %s\n"
                b"        }\n      }\n    }\n" %
                (samples, wide, 2 if wide else 1, b"eax = 1;" if samples == 4 else b""))
        text, matched = re.subn(label, lambda m: m[1] + hook, text)
        if matched:
            if b"/* 0x%08X:" % edge not in text:
                raise ValueError(f"mss: mixer-copy loop {pc:08X} has no original backedge")
            seen.add(f"mss:{pc:08X}")
            copies += matched
    if copies:
        text = (b"#include <stdint.h>\nextern int stronghold_miles_copy_bulk(uint32_t,uint32_t,uint32_t,int);\n" +
                text)
    return text, copies


def bink_rgb16(text):
    copies = 0
    for pc, edge, second, shifted in BINK_RGB_LOOPS:
        label = rb"(?m)^(L_%08X:\r?\n)(?=[^\n]*0x%08X:)" % (pc, pc)
        counter = b"MEM32(esp)" if second else b"MEM32(esp + 0x20)"
        restore = (b"uint32_t _u=MEM8(MEM32(0x30054FC0)-1), _v=MEM8(MEM32(0x30054FC4)-1); "
                   b"ebp=MEM32(0x30055408+4*_u); esi=MEM32(0x30056008+4*_v); "
                   b"ecx=MEM32(0x30055808+4*_v)+MEM32(0x30055C08+4*_u);") if shifted else b""
        hook = (b"    { uint32_t _groups = %s - 1;\n"
                b"      if (_groups && _groups <= 2048 && (!recomp_yield_hook || "
                b"(uint64_t)g_backedges + _groups < RECOMP_YIELD_EVERY) &&\n"
                b"          stronghold_bink_rgb16_bulk(_groups, %d, %d)) {\n"
                b"        %s = 1; g_backedges += _groups; %s\n      }\n    }\n" %
                (counter, second, shifted, counter, restore))
        text, matched = re.subn(label, lambda m: m[1] + hook, text)
        if matched:
            if b"goto L_%08X; /* 0x%08X:" % (pc, edge) not in text:
                raise ValueError(f"bink: 16-bit conversion loop {pc:08X} has no original backedge")
            seen.add(f"bink:{pc:08X}")
            copies += matched
    if copies:
        text = (b"#include <stdint.h>\nextern int stronghold_bink_rgb16_bulk(uint32_t,unsigned,unsigned);\n"
                + text)
    return text, copies


def mix_merge(text):
    copies = 0
    for mode, pc in enumerate(MILES_MERGE_LOOPS):
        step = (1 + ((mode >> 1) & 1)) * (1 + ((mode >> 2) & 1))
        frame = 4 + 4 * (mode & 1)
        label = rb"(?m)^(L_%08X:\r?\n)(?=[^\n]*(?:0x%08X:|_flag_a))" % (pc, pc)
        hook = (b"    { uint32_t _src_end=MEM32(0x2114C418), _dst_end=MEM32(0x2114C41C);\n"
                b"      if (_src_end>esi && _dst_end>edi && !((_src_end-esi)%%%d) && !((_dst_end-edi)%%%d)) {\n"
                b"        uint32_t _samples=(_src_end-esi)/%d, _frames=(_dst_end-edi)/%d;\n"
                b"        uint32_t _bulk=((_samples<_frames?_samples:_frames)-1)&~7u;\n"
                b"        if (_bulk>=8 && _bulk<=16384 && (!recomp_yield_hook || "
                b"(uint64_t)g_backedges+_bulk<RECOMP_YIELD_EVERY) &&\n"
                b"            stronghold_miles_merge_bulk(esi,edi,_bulk,%d)) {\n"
                b"          esi+=_bulk*%d; edi+=_bulk*%d; g_backedges+=_bulk;\n"
                b"        }\n      }\n    }\n" % (step, frame, step, frame, mode, step, frame))
        text, matched = re.subn(label, lambda m: m[1] + hook, text)
        if matched:
            seen.add(f"mss:{pc:08X}")
            copies += matched
    if copies:
        text = (b"#include <stdint.h>\nextern int stronghold_miles_merge_bulk(uint32_t,uint32_t,uint32_t,unsigned);\n"
                + text)
    return text, copies


def mix_resample(text):
    copies = 0
    for mode, pc in enumerate(MILES_RESAMPLE_LOOPS):
        label = rb"(?m)^(L_%08X:\r?\n)(?=[^\n]*0x%08X: shl edx, 0x10)" % (pc, pc)
        hook = (b"    { uint32_t _state[] = {esi, edi, edx};\n"
                b"      uint32_t _budget = !recomp_yield_hook ? 16384 : "
                b"g_backedges < RECOMP_YIELD_EVERY ? RECOMP_YIELD_EVERY-g_backedges-1 : 0;\n"
                b"      uint32_t _done = stronghold_miles_resample_bulk(_state, %d, _budget);\n"
                b"      esi = _state[0]; edi = _state[1]; edx = _state[2]; g_backedges += _done;\n"
                b"    }\n" % mode)
        text, matched = re.subn(label, lambda m: m[1] + hook, text)
        if matched:
            seen.add(f"mss:{pc:08X}")
            copies += matched
    if copies:
        text = (b"#include <stdint.h>\nextern uint32_t stronghold_miles_resample_bulk(uint32_t *,unsigned,uint32_t);\n"
                + text)
    return text, copies


def mix_upsample(text):
    labels = [(mode, pc, b"L_%08X" % pc) for mode, pc in enumerate(MILES_UPSAMPLE_LOOPS)]
    if not any(label + b":" in text for _, _, label in labels):
        return text, 0

    copies = 0

    def transform(match):
        nonlocal copies
        body = match[0]
        for mode, pc, label in labels:
            entry = re.search(rb"(?m)^" + label + rb":\r?\n", body)
            if entry is None:
                continue
            before, after = body[:entry.end()], body[entry.end():]
            after, branches = re.subn(rb"goto " + label + rb";", b"goto " + label + b"_scalar;", after)
            if branches != 2:
                raise ValueError(f"mss: linear mixer loop {pc:08X} has {branches} backedges")
            hook = b"""    {
        uint32_t budget = !recomp_yield_hook ? 16384u :
            g_backedges < RECOMP_YIELD_EVERY ? RECOMP_YIELD_EVERY - g_backedges - 1u : 0;
        uint32_t state[] = {esi, edi, edx, ecx};
        uint32_t edges = stronghold_miles_upsample(state, %uu, budget);
        esi = state[0]; edi = state[1]; edx = state[2]; ecx = state[3];
        g_backedges += edges;
    }
""" % mode
            body = before + hook + label + b"_scalar:\n" + after
            seen.add(f"mss:{pc:08X}")
            copies += 1
        return body

    text = re.sub(rb"(?ms)^void sub_[0-9A-F]{8}\(void\) \{.*?^\}", transform, text)
    decl = (b"#include <stdint.h>\n__attribute__((visibility(\"hidden\"))) "
            b"uint32_t stronghold_miles_upsample(uint32_t *,unsigned,uint32_t);\n")
    return (decl + text, copies) if copies else (text, 0)


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
