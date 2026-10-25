"""Execute the original WM_KEYDOWN/F1 freebuild branch in both pinned x86 PEs."""
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from test_touch import Original
from unicorn.x86_const import UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EIP, UC_X86_REG_ESP


LAYOUT = {
    "game": dict(wndproc=0x4B3410, special=0xF5FD98, shift=0xF5FD94,
                 mode=0x2BB8EE8, state=0x2BB8E80, gate=0x46BE30,
                 fallback=0x4B5589, command=0x248C3E8, payload=0x25F706C),
    "classic": dict(wndproc=0x4B3280, special=0xF5F938, shift=0xF5F934,
                    mode=0x2125A08, state=0x21259A0, gate=0x46BC50,
                    fallback=0x4B53F9, command=0x1A55398, payload=0x1B63B8C),
}


def main():
    results = []
    for edition, d in LAYOUT.items():
        for mode, gate_allowed in [(v, True) for v in (0, 1, 2, 3, 4, 6)] + [(3, False)]:
            original = Original(edition)
            original.w(d["special"], 0)
            original.w(d["shift"], 0)
            original.w(d["mode"], mode)
            original.w(d["state"] + 12, 0)
            commands, gates = [], []

            def hook(pc):
                u = original.u
                if pc == d["gate"]:
                    gates.append(u.reg_read(UC_X86_REG_ECX))
                    stack = u.reg_read(UC_X86_REG_ESP)
                    u.reg_write(UC_X86_REG_EAX, int(gate_allowed))
                    u.reg_write(UC_X86_REG_EIP, original.r(stack))
                    u.reg_write(UC_X86_REG_ESP, stack + 4)
                elif pc == original.d["queue"]:
                    commands.append((u.reg_read(UC_X86_REG_ECX), original.r(u.reg_read(UC_X86_REG_ESP) + 4)))
                    u.emu_stop()
                elif pc == d["fallback"]:
                    u.emu_stop()

            # Execute unmodified handler from entry, not just the decompiled leaf.
            original.run(d["wndproc"], (0x10010, 0x100, 0x70, 0x003B0001), hook=hook)
            should_open = mode == 3 and gate_allowed
            assert commands == ([(d["command"], 14)] if should_open else []), (edition, mode, commands)
            assert gates == ([d["state"]] if mode == 3 else []), (edition, mode, gates)
            if should_open:
                assert original.r(d["payload"]) == 1  # original F1 key-derived payload
            results.append(dict(edition=edition, original_sha256=original.sha, mode=mode,
                                permission_gate=gate_allowed, command=14 if should_open else None))
    output = ROOT / "generated/freebuild-f1-selfcheck.json"
    output.write_text(json.dumps({"status": "PASS", "cases": results,
        "scope": "Original WM_KEYDOWN VK_F1=0x70 through original branch and command14 queue. Only game permission predicate is stubbed; no PC game process or Android input is started."}, indent=2) + "\n")
    print(f"Original Classic/Extreme WM_KEYDOWN F1 -> mode3 permission gate -> command14: PASS ({len(results)} cases)")


if __name__ == "__main__":
    main()
