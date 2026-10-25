"""Hash-pinned corrections of original game defects, in both editions.

header_name: the save/map header reader (Classic 475DC0, Extreme 475FB0)
reads the previewed file's name into its own narrow buffer, then converts the
stale load/save buffer (Classic 2126ECA, Extreme 2BBA3AA) over the current map
name (21266F8 / 2BB9BD8). Opening the in-game Load dialog therefore empties the
name that "Restart mission" reloads in Castle Builder, which then starts on a
blank map. Nothing else depends on that conversion: the buffer is written only
by save and load, which also set the current name themselves. The call is
skipped; the callee pops its two arguments (retn 8), so the caller does too.
"""
import re

seen = set()
LAYOUT = {
    "game": dict(header_name=(0x4762B2, 0x46B3E0)),
    "classic": dict(header_name=(0x4760C2, 0x46B200)),
}


def required(module):
    return {f"{module}:header_name"} if module in LAYOUT else set()


def apply(text, module):
    layout = LAYOUT.get(module)
    if layout is None:
        return text, 0
    pc, callee = layout["header_name"]
    if b"/* 0x%08X:" % pc not in text:
        return text, 0
    pattern = rb"^(    )RECOMP_CALL\(sub_%08X\);( /\* 0x%08X: call 0x%x \*/)(\r?)$" % (callee, pc, callee)
    replacement = b"esp += 8; /* stale header name: skipped, retn 8 */"
    text, count = re.subn(pattern, lambda m: m[1] + replacement + m[2] + m[3], text, flags=re.M)
    if not count:
        raise ValueError(f"{module}:{pc:08X}: original header-name conversion changed")
    seen.add(f"{module}:header_name")
    return text, count


if __name__ == "__main__":
    from pathlib import Path
    root = Path(__file__).resolve().parents[1]
    for module in LAYOUT:
        seen.clear()
        total = 0
        for path in (root / "generated" / f"{module}-x86").glob("recomp_*.c"):
            text, count = apply(path.read_bytes(), module)
            total += count
            if count:
                assert b"RECOMP_CALL(sub_%08X); /* 0x%08X" % (LAYOUT[module]["header_name"][1],
                                                              LAYOUT[module]["header_name"][0]) not in text
        assert not (required(module) - seen), sorted(required(module) - seen)
        print(f"{module}: header-name conversion skipped in {total} lifted copies PASS")
