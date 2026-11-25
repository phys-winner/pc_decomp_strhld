"""Bind the SHA-pinned map/save block codec to the native implementations."""
import re

ENTRIES = {"classic": 0x472D90, "game": 0x472FA0, "stronghold": 0x461140}
seen = set()


def required(module):
    return {(module, "encode"), (module, "decode")} if module in ENTRIES else set()


def apply(text, module):
    address = ENTRIES.get(module)
    if address is None:
        return text
    for kind, pc, call in (
        ("decode", address, b"stronghold_map_decode(); return;"),
        ("encode", address - 0xE0, b"if (stronghold_map_encode()) return;"),
    ):
        label = (rb"(?m)^(L_%08X:\r?\n)"
                 rb"(    _flag_a = \(uint32_t\)\(esp\); _flag_b = \(uint32_t\)\(0x34u\);\r?\n"
                 rb"    _cf = \(uint32_t\)CMP_B\(_flag_a, _flag_b\);\r?\n"
                 rb"    esp = esp - 0x34u; /\* 0x%08X: sub esp, 0x34 \*/)") % (pc, pc)
        text, changed = re.subn(label, lambda m: m[1] + b"    RECOMP_FLAGS_OUT(); RECOMP_REGS_OUT(); " + call + b"\n" + m[2], text)
        if changed:
            seen.add((module, kind))
            declaration = b"extern void stronghold_map_decode(void);" if kind == "decode" else b"extern int stronghold_map_encode(void);"
            text = declaration + b"\n" + text
    return text
