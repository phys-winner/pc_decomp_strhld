"""Inspect ELF64 AArch64 program headers for a target host page size."""
import struct


def inspect(data, page_size=16384):
    assert page_size >= 4096 and page_size & (page_size - 1) == 0
    assert data[:6] == b"\x7fELF\x02\x01", "Expected little-endian ELF64"
    assert struct.unpack_from("<H", data, 18)[0] == 183, "Expected AArch64"
    offset = struct.unpack_from("<Q", data, 32)[0]
    entry_size, count = struct.unpack_from("<HH", data, 54)
    assert entry_size >= 56 and offset + entry_size * count <= len(data), "Invalid program header table"
    loads, relros = [], []
    down = lambda n: n // page_size * page_size
    up = lambda n: (n + page_size - 1) // page_size * page_size
    for index in range(count):
        kind, flags, file_at, address, _, file_size, memory_size, alignment = struct.unpack_from("<IIQQQQQQ", data, offset + index * entry_size)
        if kind not in (1, 0x6474E552):
            continue
        assert file_at + file_size <= len(data) and file_size <= memory_size, "Segment exceeds its file/memory range"
        row = {"index": index, "flags": flags, "offset": file_at, "vaddr": address,
               "filesz": file_size, "memsz": memory_size, "align": alignment,
               "page_start": down(address), "page_end": up(address + memory_size)}
        if kind == 1:
            assert alignment >= page_size and alignment & (alignment - 1) == 0, "PT_LOAD alignment below host page size"
            assert (address - file_at) % alignment == 0, "PT_LOAD offset/vaddr are not congruent"
            assert not flags & 1 or not flags & 2, "Writable executable PT_LOAD"
            loads.append(row)
        else:
            assert (address + memory_size) % page_size == 0, "GNU_RELRO end is not host-page aligned"
            relros.append(row)
    assert loads and relros, "Missing PT_LOAD or GNU_RELRO"
    for i, left in enumerate(loads):
        for right in loads[i + 1:]:
            overlap = max(left["page_start"], right["page_start"]) < min(left["page_end"], right["page_end"])
            assert not overlap or left["flags"] == right["flags"], "PT_LOAD protections share a host page"
    for relro in relros:
        assert any(load["flags"] & 2 and load["vaddr"] <= relro["vaddr"] and
                   relro["page_end"] <= load["page_end"] for load in loads), "GNU_RELRO outside a writable mapping"
    return {"status": "PASS", "page_size": page_size, "pt_load": loads, "gnu_relro": relros,
            "checks": ["PT_LOAD p_align >= host page", "offset/vaddr congruence", "no mixed-protection host-page overlap",
                       "GNU_RELRO host-page-aligned end contained in writable mapping"]}
