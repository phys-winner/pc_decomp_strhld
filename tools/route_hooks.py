"""Optimize the pinned Crusader reachability queue without changing search order."""
import re

LAYOUT = {
    "classic": (0x498B20, 0x1FAA9C8, 0x1F835A8, 0x1F5C188, 0x1D35FF8, 0x1DD3848, 0xB3ADA8, 0xB4AC48),
    "game": (0x498CB0, 0x2A3DEA8, 0x2A16A88, 0x29EF668, 0x27C94D8, 0x2866D28, 0xB3AF38, 0xB4ADD8),
}
seen = set()


def required(module):
    return {module} if module in LAYOUT else set()


def apply(text, module):
    addresses = LAYOUT.get(module)
    if addresses is None:
        return text
    base, visited, depth, connections, flags, height, direction, delta = addresses
    body = re.search(rb"(?ms)^void sub_%08X\(void\) \{.*?^\}" % base, text)
    if not body:
        return text
    start, fourth = base + 0x1D0, base + 0x4D4
    anchor = rb"(?m)^(L_%08X:\r?\n)(?=    ecx = MEM32\(ecx \+ \(-0x4\)\);)" % start
    if b"L_%08X:\r\n    ecx = MEM32(esp + 0x24);" % fourth not in body[0]:
        raise ValueError(f"{module}: route queue continuation changed")
    block = rb"""    /* The fourth direction still sets the lifted registers and flags before a yield. */
    if (MEM32(esi + 0x1BB20u) < 80400u) {
        for (unsigned _i = 0; _i < 3; ++_i) {
            uint32_t _neighbor = ebx + MEM32(ecx - 4u + _i * 4u);
            uint32_t _epoch = MEM32(esi + 4u);
            if ((uint32_t)(int32_t)(int16_t)MEM16(0x%Xu + 2u * _neighbor) == _epoch) continue;
            unsigned _direction = eax + _i;
            if (!(MEM8(0x%Xu + ebx) & MEM8(0x%Xu + _direction))) {
                if (_i & 1u) continue;
                edi = MEM32(0x%Xu + 4u * _neighbor);
                if ((edi & 0x4A5014B1u) || MEM32(esp + 0x18u) ||
                    MEM16(0x%Xu + 2u * _neighbor) || !((edi ^ MEM32(esp + 0x1Cu)) & 0x100u))
                    continue;
            }
            MEM16(0x%Xu + 2u * _neighbor) = (uint16_t)(MEM32(esi + 0x1BB14u) + 1);
            MEM16(0x%Xu + 2u * _neighbor) = (uint16_t)_epoch;
            edi = MEM32(esi + 0x1BB20u);
            MEM16(esi + 0x917A0u + 2u * edi) = (uint16_t)(MEM16(esp + 0x20u) + MEM16(0x%Xu + 8u * _direction));
            MEM16(esi + 0x6A380u + 2u * edi) = (uint16_t)(ebp + MEM16(0x%Xu + 8u * _direction));
            MEM32(esi + 0x1BB40u + 4u * edi) = _neighbor;
            MEM32(esi + 0x1BB20u) = edi == 80399u ? 0 : edi + 1;
        }
        goto L_%08X;
    }
""" % (visited, connections, direction, flags, height, depth, visited, delta, delta + 4, fourth)
    patched, count = re.subn(anchor, lambda m: m[1] + block, body[0])
    if count != 1:
        raise ValueError(f"{module}: route queue entry changed")
    seen.add(module)
    return text[:body.start()] + patched + text[body.end():]
