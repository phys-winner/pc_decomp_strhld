"""16KB loader audit accepts current ELFs and rejects broken header layouts."""
from pathlib import Path
import struct
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from verify_elf_pages import inspect


def rejected(data, expected):
    try:
        inspect(data)
    except AssertionError as error:
        assert expected in str(error), str(error)
    else:
        raise AssertionError("Broken program header unexpectedly accepted")


for name in ("libstronghold.so", "libstronghold_classic.so"):
    data = (ROOT / "build/arm64" / name).read_bytes()
    result = inspect(data)
    assert result["page_size"] == 16384
    table = struct.unpack_from("<Q", data, 32)[0]
    stride = struct.unpack_from("<H", data, 54)[0]
    load = table + stride * result["pt_load"][0]["index"]
    relro = table + stride * result["gnu_relro"][0]["index"]
    broken = bytearray(data)
    struct.pack_into("<Q", broken, load + 48, 4096)
    rejected(broken, "alignment below")
    broken = bytearray(data)
    struct.pack_into("<Q", broken, load + 16, 1)
    rejected(broken, "not congruent")
    broken = bytearray(data)
    struct.pack_into("<I", broken, load + 4, 7)
    rejected(broken, "Writable executable")
    broken = bytearray(data)
    struct.pack_into("<Q", broken, relro + 40, result["gnu_relro"][0]["memsz"] + 4096)
    rejected(broken, "RELRO end")
    print(name + ": 16KB PT_LOAD/RELRO PASS; four malformed layouts rejected")
