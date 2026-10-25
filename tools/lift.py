"""Lift the original Stronghold/Crusader executables and their Miles/Bink libraries to C."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import sys

from lift_core import ROOT, collect_functions, emit_functions, emit_dispatch
from tools.pe.pe_analyze import analyze_pe, build_iat_map
from tools.disasm.disasm32 import Disassembler

MODULES = {"game": "Stronghold_Crusader_Extreme.exe", "classic": "Stronghold Crusader.exe",
           "stronghold": "Stronghold.exe", "mss": "Mss32.dll", "bink": "binkw32.dll"}
EDITIONS = {"extreme": ("game", "mss", "bink"), "classic": ("classic", "mss", "bink"),
            "stronghold": ("stronghold", "mss", "bink")}


def game_directory(config, module):
    """The original Stronghold HD is a separate installation with its own directory."""
    for edition in config.get("editions", {}).values():
        if edition.get("executable") == MODULES[module] and edition.get("game_directory"):
            return edition["game_directory"]
    return config["game_directory"]


class SafeDisassembler(Disassembler):
    def read_bytes(self, address, size):
        # The vendor closure scanner asks beyond the final raw section.
        return super().read_bytes(address, size) or b""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("modules", nargs="*", choices=list(MODULES))
    args = parser.parse_args()
    for name in args.modules or MODULES:
        binary = ROOT / "generated/input" / MODULES[name]
        if not binary.exists():
            if not (ROOT / "port.json").is_file():
                parser.error("copy port.example.json to port.json and set your original game directories")
            config = json.loads((ROOT / "port.json").read_text(encoding="utf-8"))
            original = ROOT / game_directory(config, name) / MODULES[name]
            expected = json.loads((ROOT / "config/inputs.json").read_text())[MODULES[name]]
            if hashlib.sha256(original.read_bytes()).hexdigest() != expected:
                raise ValueError(f"{original.name}: original SHA256 mismatch")
            binary.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(original, binary)
        raw = binary.read_bytes()
        digest = hashlib.sha256(raw).hexdigest()
        expected = json.loads((ROOT / "config/inputs.json").read_text())[MODULES[name]]
        if digest != expected:
            raise ValueError(f"{binary.name}: input SHA256 mismatch")
        output = ROOT / f"generated/{name}-x86"
        old_report = output / "report.json"
        catalog_path = output / "catalog.json"
        if catalog_path.exists() and old_report.exists() and json.loads(old_report.read_text())["binary_sha256"] == digest:
            catalog = json.loads(catalog_path.read_text())
        else:
            info = analyze_pe(str(binary))
            decoder = SafeDisassembler(raw, info.image_base, info.sections)
            assert decoder.read_bytes(0xFFFFFFFF, 16) == b""
            seeds = [info.image_base + info.entry_point_rva]
            seeds += [info.image_base + e.rva for e in info.exports if e.rva]
            functions = decoder.find_functions(info.code_start, info.code_end, build_iat_map(info), seeds=seeds)
            catalog = [{"address": address} for address in functions]
            del functions, decoder
        extra = json.loads((ROOT / "config/seeds.json").read_text()).get(name, [])
        info, imports, decoder, functions, rejected = collect_functions(
            binary, catalog, [int(a, 0) for a in extra], label=name)
        emitted = emit_functions(output, imports, decoder, functions, label=name)
        report = {"binary_sha256": digest, "entry": hex(info.image_base + info.entry_point_rva),
                  "base": hex(info.image_base), "rejected_entries": rejected, **emitted,
                  "runtime_status": "UNVERIFIED"}
        (output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
        (output / "catalog.json").write_text(json.dumps(catalog) + "\n")
        print(f"{name}: {len(functions)} entries, {len(emitted['functions_containing_traps'])} bodies with traps", flush=True)
    # All three EXEs occupy 00400000: each library must dispatch exactly one EXE.
    import re
    for edition, modules in EDITIONS.items():
        if not all((ROOT / f"generated/{name}-x86/recomp_funcs.h").exists() for name in modules):
            continue
        addresses, headers = [], []
        for name in modules:
            catalog = ROOT / f"generated/{name}-x86/recomp_funcs.h"
            addresses += [int(a, 16) for a in re.findall(r"void sub_([0-9A-F]+)\(void\);", catalog.read_text())]
            headers.append(f"../{name}-x86/recomp_funcs.h")
        dispatch = ROOT / f"generated/dispatch-{edition}/recomp_dispatch.c"
        dispatch.parent.mkdir(parents=True, exist_ok=True)
        emit_dispatch(dispatch, addresses, headers)


if __name__ == "__main__":
    main()
