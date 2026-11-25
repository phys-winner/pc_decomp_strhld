"""Bind the pinned engines' screen calls directly to the Android renderer."""
import re


# Each row names the same screen operation in Crusader, Extreme and HD.
OPERATIONS = {
    "lock": ("4090E8 409119 409168 409195 470314 4703CB",
             "4090E8 409119 409168 409195 470524 4705DB",
             "408588 4085BB 40860E 408646 45E97E 45EA39"),
    "restore": ("4090FC 40917C 468021 46803C 468056 468070",
                "4090FC 40917C 468221 46823C 468256 468270",
                "40859C 40862B 458FE1 458FFC 459016 459030"),
    "unlock": ("4091DB 470339 470402", "4091DB 470549 470612", "40868D 45E9A3 45EA5E"),
    "configure": ("467EEA 47014B", "4680EA 47035B", "458EAA 45E7AB"),
    "release": ("467EFC 467F14 467F2C 467F44", "4680FC 468114 46812C 468144", "458EBC 458ED4 458EEC 458F04"),
    "close": ("467F66", "468166", "458F26"),
    "acquire_dc": ("4680A8 4680BD", "4682A8 4682BD", "459068 45907D"),
    "capabilities": ("4700A1", "4702B1", "45E701"),
    "modes": ("4700CA", "4702DA", "45E72A"),
    "set_mode": ("470164", "470374", "45E7C4"),
    "allocate": ("4701D2 470238 4702D4 47038F", "4703E2 470448 4704E4 47059F", "45E832 45E898 45E942 45E9F9"),
    "back_buffer": ("470279", "470489", "45E8D5"),
    "describe": ("47043E", "47064E", "45EA9A"),
    "copy_at": ("470704", "470914", "45ED64"),
    "copy": ("470764 470800 470899 4709AC 470A65 470B78 470C31 470C78 470D6A 470E6B",
             "470974 470A10 470AA9 470BBC 470C75 470D88 470E41 470E88 470F7A 47107B",
             "45EDC4 45EE60 45EEF9 45F00C 45F0C5 45F1D8 45F291 45F2D8 45F3CA 45F4CB"),
    "swap": ("4709CD 470D85 470E85", "470BDD 470F95 471095", "45F02D 45F3E5 45F4E5"),
}
MODULES = ("classic", "game", "stronghold")
CALLS = {
    module: {int(pc, 16): method for method, rows in OPERATIONS.items() for pc in rows[i].split()}
    for i, module in enumerate(MODULES)
}
CALLS["bink"] = {0x3000390E: "format"}
OPEN = {"classic": (0x57DC94, 0x59F010), "game": (0x57E0E4, 0x59F010), "stronghold": (0x533C88, 0x554010)}
RELEASE_DC = {"classic": (0x4680EB, 0x4680FC), "game": (0x4682EB, 0x4682FC), "stronghold": (0x4590AB, 0x4590BC)}
LEGACY_CAPABILITY = {"classic": 0x46F930, "game": 0x46FB10}

# These reads fed the covered calls' virtual targets and have no engine data consumers.
CRUSADER_LOADS = """
4090DC 4090DE 4090F6 4090F8 40910B 40910D 40915C 40915E 409176 409178 409187 409189 4091D1 4091D3
467EE1 467EE7 467EF6 467EF8 467F0E 467F10 467F26 467F28 467F3E 467F40 467F60 467F62
46801B 46801D 468036 468038 468050 468052 46806A 46806C 4680A2 4680A5 4680B7 4680BA 4680E5 4680EB 4680F6 4680FC
47009A 47009E 4700B9 4700BB 470140 470148 470157 470161 4701CB 4701CD 470231 470235 470272 470274
4702CD 4702D1 470305 470307 47032E 470330 470387 47038A 4703BC 4703BE 4703F7 4703F9 470437 47043B
4706EC 4706EE 470747 470761 4707E3 4707FD 47087C 470896 47098F 470991 4709C5 4709C7 470A48 470A62 470B5B 470B5D
470C13 470C2E 470C5A 470C5C 470D4D 470D67 470D7C 470D7E 470E4E 470E68 470E7C 470E7E
"""
HD_LOADS = """
40857B 40857D 408596 408598 4085AB 4085AD 408601 408603 408625 408627 408636 408638 408682 408684
458EA1 458EA7 458EB6 458EB8 458ECE 458ED0 458EE6 458EE8 458EFE 458F00 458F20 458F22
458FDB 458FDD 458FF6 458FF8 459010 459012 45902A 45902C 459062 459065 459077 45907A 4590A5 4590AB 4590B6 4590BC
45E6FA 45E6FE 45E719 45E71B 45E7A0 45E7A8 45E7B7 45E7C1 45E82B 45E82D 45E891 45E895 45E8CE 45E8D0
45E93B 45E93F 45E96F 45E971 45E998 45E99A 45E9F1 45E9F4 45EA2A 45EA2C 45EA53 45EA55 45EA93 45EA97
45ED4C 45ED4E 45EDA7 45EDC1 45EE43 45EE5D 45EEDC 45EEF6 45EFEF 45EFF1 45F025 45F027 45F0A8 45F0C2 45F1BB 45F1BD
45F273 45F28E 45F2BA 45F2BC 45F3AD 45F3C7 45F3DC 45F3DE 45F4AE 45F4C8 45F4DC 45F4DE
"""
LOADS = {
    "classic": {int(pc, 16) for pc in CRUSADER_LOADS.split()},
    "stronghold": {int(pc, 16) for pc in HD_LOADS.split()},
    "bink": {0x300038FE},
}
LOADS["game"] = {pc if pc < 0x467EE1 else pc + (0x200 if pc < 0x47009A else 0x210) for pc in LOADS["classic"]}

CALL = re.compile(rb"(?m)^    RECOMP_ICALL\([^\r\n]+\); /\* 0x([0-9A-F]{8}): call [^\r\n]+\*/")
LOAD = re.compile(rb"(?m)^(    e\w+ = )MEM32\([^\r\n]+\);( /\* 0x([0-9A-F]{8}): mov e\w+, dword ptr \[[^\r\n]+\*/)")
seen = set()


def required(module):
    calls = (*CALLS.get(module, ()), *RELEASE_DC.get(module, ()), *OPEN.get(module, ())[:1])
    return ({(module, "call", pc) for pc in calls} |
            {(module, "load", pc) for pc in LOADS.get(module, ())} |
            ({(module, "probe", LEGACY_CAPABILITY[module])} if module in LEGACY_CAPABILITY else set()))


def apply(text, module):
    calls = CALLS.get(module)
    if not calls:
        return text
    original = text

    def direct(match):
        pc = int(match[1], 16)
        if pc not in calls:
            return match[0]
        seen.add((module, "call", pc))
        return b"    RECOMP_CALL(stronghold_render_" + calls[pc].encode() + b");" + match[0][match[0].index(b" /*"):]

    def drop_load(match):
        pc = int(match[3], 16)
        if pc not in LOADS[module]:
            return match[0]
        seen.add((module, "load", pc))
        return match[1] + b"0;" + match[2]

    text = CALL.sub(direct, text)
    for pc in RELEASE_DC.get(module, ()):
        pattern = rb"(/\* 0x%08X: mov eax, dword ptr \[ecx \+ 0x68\] \*/\r?\n)    \{ _itail_tgt = \(uint32_t\)\(eax\); goto _ljump; \}" % pc
        text, count = re.subn(pattern, rb"\1    RECOMP_FLAGS_OUT(); RECOMP_REGS_OUT(); stronghold_render_release_dc(); return;", text)
        if count:
            seen.add((module, "call", pc))
    if module in OPEN:
        entry, iat = OPEN[module]
        pattern = (rb"(L_%08X:\r?\n)    \{ _itail_tgt = \(uint32_t\)\(MEM32\(0x%X\)\); goto _ljump; \}"
                   % (entry, iat))
        text, count = re.subn(pattern, rb"\1    RECOMP_FLAGS_OUT(); RECOMP_REGS_OUT(); stronghold_render_open(); return;", text)
        if count:
            seen.add((module, "call", entry))
    if module in LEGACY_CAPABILITY:
        # Crusader gates its LAN menu on the DirectDraw 7 interface probe.
        # The native renderer already provides the screen contract queried here.
        entry = LEGACY_CAPABILITY[module]
        pattern = rb"(L_%08X:\r?\n)(    PUSH32\(esp, ecx\); /\* 0x%08X: push ecx \*/)" % (entry, entry)
        text, count = re.subn(pattern, rb"\1    eax=0x700u; esp+=4; RECOMP_FLAGS_OUT(); RECOMP_REGS_OUT(); return;\n\2", text)
        if count:
            seen.add((module, "probe", entry))
    text = LOAD.sub(drop_load, text)
    return b'#include "renderer.h"\n' + text if text != original else text
