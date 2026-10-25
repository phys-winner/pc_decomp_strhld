"""Execute the original chat-history callback in both pinned x86 images.

This checks the cdecl ABI, direction, clamps and pause/menu guards. It does
not run Android UI or claim that a chat body is visible in a particular game.
"""
import hashlib
import json
from pathlib import Path
import struct

import pefile
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32
from unicorn.x86_const import (UC_X86_REG_EBP, UC_X86_REG_EBX,
                              UC_X86_REG_EDI, UC_X86_REG_ESI, UC_X86_REG_ESP)


ROOT = Path(__file__).resolve().parents[1]
LAYOUTS = (
    ("extreme", "Stronghold_Crusader_Extreme.exe", 0x442980, 0x25F50D4,
     (0xB982A4, 0x1163EAC), 0x5EBC58, 0x42ADC0, 0x4809F3,
     0x248CA00, 0x2BB8E2C),
    ("classic", "Stronghold Crusader.exe", 0x4427B0, 0x1B61BF4,
     (0xB98124, 0x1163A4C), 0x5EBD98, 0x42AD40, 0x480803,
     0x1A559B0, 0x212594C),
)


def main():
    inputs = json.loads((ROOT / "config/inputs.json").read_text())
    editions = []
    for edition, name, callback, history, guards, body, renderer, pitch, session, dialog in LAYOUTS:
        raw = (ROOT / "generated/input" / name).read_bytes()
        digest = hashlib.sha256(raw).hexdigest()
        assert digest == inputs[name], f"{edition}: original image changed"
        pe = pefile.PE(data=raw)
        base = pe.OPTIONAL_HEADER.ImageBase
        record = pe.get_data(body - base, 80)
        assert struct.unpack_from("<I4i", record) == (0x02000003, 10, 428, 530, 107)
        assert struct.unpack_from("<i", record, 24)[0] == -3
        # Original renderer: sub edi,17 after each ordinary message line.
        assert pe.get_data(pitch - base, 3) == bytes.fromhex("83 ef 11")
        uc = Uc(UC_ARCH_X86, UC_MODE_32)
        uc.mem_map(base, (pe.OPTIONAL_HEADER.SizeOfImage + 4095) & ~4095)
        uc.mem_write(base, pe.get_memory_mapped_image())
        stack, stop = 0x70000000, 0x6FFF0000
        uc.mem_map(stack, 4096)
        uc.mem_map(stop, 4096)
        preserved = (UC_X86_REG_EBX, UC_X86_REG_ESI, UC_X86_REG_EDI, UC_X86_REG_EBP)
        cases = []
        for initial in (0, 1, 14, 15):
            for argument in (-1, -2):
                for guard, menu in ((0, 0), (1, 0), (0, 1)):
                    for address, value in ((history, initial), (guards[0], guard), (guards[1], menu)):
                        uc.mem_write(address, struct.pack("<I", value))
                    sp = stack + 0xF00
                    uc.mem_write(sp, struct.pack("<Ii", stop, argument))
                    uc.reg_write(UC_X86_REG_ESP, sp)
                    for index, register in enumerate(preserved):
                        uc.reg_write(register, 0x12340000 + index)
                    uc.emu_start(callback, stop, count=5000)
                    result = struct.unpack("<I", uc.mem_read(history, 4))[0]
                    expected = initial if guard or menu else max(0, min(15, initial + (1 if argument == -1 else -1)))
                    assert result == expected, (edition, initial, argument, guard, menu, result)
                    assert uc.reg_read(UC_X86_REG_ESP) == sp + 4, f"{edition}: cdecl stack"
                    for index, register in enumerate(preserved):
                        assert uc.reg_read(register) == 0x12340000 + index
                    cases.append(dict(start=initial, arg=argument, guard=guard,
                                      menu=menu, result=result, status="PASS"))
        editions.append(dict(
            edition=edition, original_sha256=digest, body=hex(body),
            body_rect=[10, 428, 530, 107], body_arg=-3, render=hex(renderer),
            original_callback=hex(callback),
            callback_abi="cdecl1arg; caller pops argument; EBX/ESI/EDI/EBP preserved",
            history_offset=hex(history), older_arg=-1, newer_arg=-2, min=0, max=15,
            nominal_step_pixels=17, wrapped_message_extra_pixels=18,
            guards=[hex(address) for address in guards], original_cases=cases,
            visibility=dict(session_not_99=hex(session), dialog=hex(dialog),
                            allowed_dialogs=[-1, 42, 43, 118, 119, 120],
                            source="42ADC0/42AD40 early returns plus pure427310/427290",
                            scope="Static original-renderer audit; callback execution alone does not test visibility.")))
        print(f"{edition}: 24 original-x86 clamp/direction/guard/ABI cases; body descriptor and17px renderer pitch: PASS")
    proof = dict(
        status="PASS", original_x86_cases=sum(len(e["original_cases"]) for e in editions),
        scope="Original x86 callback executed with Unicorn; no production patch or phone operation.",
        regression_script="tools/test_chat_history.py",
        regression_script_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        direction="Positive finger dy -> older arg-1 -> offset+1. Negative finger dy -> newer arg-2 -> offset-1.",
        renderer="Starts at ring head minus history offset (modulo20), draws backward at most5 display lines; ordinary message line pitch17px, wrapped message adds18px.",
        editions=editions)
    (ROOT / "generated/touch-analysis/chat-history-proof.json").write_text(json.dumps(proof, indent=2) + "\n")


if __name__ == "__main__":
    main()
