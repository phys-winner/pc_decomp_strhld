"""Hash-pinned Stronghold decision seams for native Android touch.

Hooks select paths in the original code, including native control of the
single-player lord; order payloads, destination validity and the command queue stay original. Every
instruction match applies to direct and lifted fragment bodies alike.
"""
import re

seen = set()
LAYOUT = {
    "game": dict(prepare=0x57D14C, after=0x57D2F5, context=0x434C80,
                 terrain=0x443DF0, place=0x445580, inspect=0x434700, wall=0x438280, demolish=0x438B80,
                 inspect_gate=0x434A37, inspect_check=0x434A3F,
                 inspect_release=0x434A82, inspect_do=0x434A88,
                 build_cancel=0x434994, build_cancel_do=0x4349CF,
                 inspect_range=(0x434700,0x434BC1), selection=0x434D79,
                 rectangle=0x434EE3, selected=0x434F50, lord_group=0x434F6C, order=0x43713A,
                 lord_lock=0x4370F3, lord_allowed=0x437136,
                 fortification_gate=0x436F3A, fortification_allowed=0x43701A,
                 tile_flags=0x27C94D8, tile_owner=0x292B1C8, player=0x25F874C,
                 dispatch=0x4370A3, ability=0x43532B, ability_do=0x435338,
                 cancel=0x4350B9, cancel_do=0x4350DD,
                 preview=0x4ED31A, preview_end=0x4ED599,
                 input=0xF6A258, map=0x2DD78A0, rect=0x468880,
                 ui_ranges=((0x4AAAA0,0x4AAC90),(0x468470,0x4684B0),(0x4F5000,0x4F5740),
                            (0x4634F0,0x463685)),
                 picker=0x4E91E7, picker_world=0x4E9267,
                 renderer=(0x4E9A40,0x4ED5C2), sprite_hit=0x4634F0,
                 ui_redraw=0x57D348, ui_full=0x57D465,
                 tooltip=(0x4F7210,0x4F7706), help_flag=0x2BB8F4C,
                 context_range=(0x434C80,0x437C61),
                 inspection_buttons=(0x440660,0x440738),
                 placement_ranges=((0x443DF0,0x444660),(0x445580,0x446B90),
                                   (0x438070,0x438490),(0x438580,0x438ABE),(0x438B80,0x438F10))),
    "classic": dict(prepare=0x57CCFC, after=0x57CEA5, context=0x434A70,
                    terrain=0x443BB0, place=0x445340, inspect=0x4344F0, wall=0x438070, demolish=0x438970,
                    inspect_gate=0x434827, inspect_check=0x43482F,
                    inspect_release=0x434872, inspect_do=0x434878,
                    build_cancel=0x434784, build_cancel_do=0x4347BF,
                    inspect_range=(0x4344F0,0x4349B1), selection=0x434B69,
                    rectangle=0x434CD3, selected=0x434D40, lord_group=0x434D5C, order=0x436F2A,
                    lord_lock=0x436EE3, lord_allowed=0x436F26,
                    fortification_gate=0x436D2A, fortification_allowed=0x436E0A,
                    tile_flags=0x1D35FF8, tile_owner=0x1E97CE8, player=0x1B6526C,
                    dispatch=0x436E93, ability=0x43511B, ability_do=0x435128,
                    cancel=0x434EA9, cancel_do=0x434ECD,
                    preview=0x4ECF6A, preview_end=0x4ED1E9,
                    input=0xF69DF8, map=0x23443C0, rect=0x468680,
                    ui_ranges=((0x4AA910,0x4AAB00),(0x468270,0x4682B0),(0x4F4C40,0x4F5380),
                               (0x4632C0,0x463455)),
                    picker=0x4E8E37, picker_world=0x4E8EB7,
                    renderer=(0x4E9690,0x4ED212), sprite_hit=0x4632C0,
                    ui_redraw=0x57CEF8, ui_full=0x57D015,
                    tooltip=(0x4F6E50,0x4F7346), help_flag=0x2125A6C,
                    context_range=(0x434A70,0x437A51),
                    inspection_buttons=(0x440450,0x440528),
                    placement_ranges=((0x443BB0,0x444420),(0x445340,0x446950),
                                      (0x437E60,0x438280),(0x438370,0x4388AE),(0x438970,0x438D00))),
}
KEYS = {"prepare", "after", "context", "terrain", "place", "selection",
        "order", "ability", "cancel", "preview", "world_point", "pick_point",
        "legacy", "placement", "ui_point", "inspect", "inspect_gate",
        "inspect_release", "build_cancel", "picker", "wall", "demolish", "ui_button", "sprite_hit",
        "ui_redraw", "ui_help", "inspection_button", "lord_group", "lord_lock", "fortification"}
PC = re.compile(rb"/\* 0x([0-9A-F]{8}):")


def required(module):
    return {f"{module}:{key}" for key in KEYS} if module in LAYOUT else set()


def apply(text, module):
    if module not in LAYOUT:
        return text, 0
    layout = LAYOUT[module]
    output, count, pc = [], 0, 0

    def record(key):
        nonlocal count
        seen.add(f"{module}:{key}")
        count += 1

    for line in text.splitlines(keepends=True):
        match = PC.search(line)
        if match:
            pc = int(match[1], 16)
            # Only the first line of an instruction has its original PC.
            if pc == layout["sprite_hit"]:
                # The original alpha hit test indexes its mask before checking bounds.
                output.append(("    if ((int32_t)stronghold_touch_ui_x(MEM32(0x%Xu)) < 0 || "
                               "(int32_t)stronghold_touch_ui_y(MEM32(0x%Xu)) < 0) { "
                               "eax=0; esp+=4; RECOMP_FLAGS_OUT(); RECOMP_REGS_OUT(); return; }\n" %
                               (layout["input"]+0x10,layout["input"]+0x14)).encode())
                record("sprite_hit")
            elif pc == layout["prepare"]:
                output.append(b"    RECOMP_REGS_OUT(); stronghold_touch_frame(); RECOMP_REGS_IN();\n")
                record("prepare")
            elif pc == layout["ui_redraw"]:
                target = b"goto L_%08X;" % layout["ui_full"]
                assert b"if (CMP_NE(" in line and target in line, "original full-HUD branch changed"
                output.append(b"    if (stronghold_touch_ui_redraw()) " + target + b"\n")
                record("ui_redraw")
            elif pc == layout["after"]:
                output.append(("    RECOMP_REGS_OUT(); stronghold_touch_after_world(); "
                               "stronghold_display_ui_begin(0x%08Xu); RECOMP_REGS_IN();\n" % layout["map"]).encode())
                record("after")
            elif pc in (layout[k] for k in ("context","terrain","place","inspect","wall","demolish")):
                key = next(k for k in ("context", "terrain", "place", "inspect","wall","demolish") if layout[k] == pc)
                kind = {"context": 0, "terrain": 1, "place": 2, "inspect": 3,"wall":4,"demolish":5}[key]
                output.append(("    if (stronghold_touch_callback(%d)) { esp += 4; "
                               "RECOMP_FLAGS_OUT(); RECOMP_REGS_OUT(); return; }\n" % kind).encode())
                record(key)
            elif pc == layout["selection"]:
                output.append(("    if (stronghold_touch_selection()) { esi = 107; ebx = 61; "
                               "if (stronghold_touch_selection() == 2) goto L_%08X; "
                               "goto L_%08X; }\n" % (layout["selected"], layout["rectangle"])).encode())
                record("selection")
            elif pc == layout["order"]:
                output.append(("    if (stronghold_touch_order((int32_t)esi)) { ebx = 107; ebp = 1; "
                               "goto L_%08X; }\n" % layout["dispatch"]).encode())
                record("order")
            elif pc == layout["lord_lock"]:
                assert b"MEM32(esp + 0x18)" in line, "original lord permission gate changed"
                output.append(("    if (stronghold_touch_lord_control()) goto L_%08X;\n" % layout["lord_allowed"]).encode())
                record("lord_lock")
            elif pc == layout["fortification_gate"]:
                assert (b"/* cmp MEM32(0x%X), 1 */" % (layout["map"]+108)) in line, "original raised-target gate changed"
                # The original picker leaves +84 empty for keep platforms; +96/+112 still identify the raised destination.
                output.append((
                    "    { uint32_t touch_tile = MEM32(0x%Xu);\n"
                    "      if (stronghold_touch_lord_control() && touch_tile &&\n"
                    "          (!MEM32(0x%Xu) || touch_tile == MEM32(0x%Xu)) && touch_tile == MEM32(0x%Xu) &&\n"
                    "          (MEM32(0x%Xu + touch_tile * 4u) & 0x10000300u) &&\n"
                    "          ((MEM8(0x%Xu + touch_tile) & 7u) + 1u) == MEM32(0x%Xu))\n"
                    "          goto L_%08X; }\n" %
                    (layout["map"]+96, layout["map"]+84, layout["map"]+84, layout["map"]+112,
                     layout["tile_flags"], layout["tile_owner"], layout["player"],
                     layout["fortification_allowed"])).encode())
                record("fortification")
            elif pc == layout["ability"]:
                output.append(("    if (stronghold_touch_ability()) goto L_%08X;\n" % layout["ability_do"]).encode())
                record("ability")
            elif pc == layout["cancel"]:
                output.append(("    if (stronghold_touch_cancel()) goto L_%08X;\n" % layout["cancel_do"]).encode())
                record("cancel")
            elif pc in (layout["inspect_gate"], layout["inspect_release"]):
                key = "inspect_gate" if pc == layout["inspect_gate"] else "inspect_release"
                target = layout["inspect_check"] if key == "inspect_gate" else layout["inspect_do"]
                output.append(("    if (stronghold_touch_inspect()) goto L_%08X;\n" % target).encode())
                record(key)
            elif pc == layout["build_cancel"]:
                output.append(("    if (stronghold_touch_cancel()) goto L_%08X;\n" % layout["build_cancel_do"]).encode())
                record("build_cancel")
            elif pc == layout["preview"]:
                output.append(("    if (!stronghold_touch_known()) goto L_%08X;\n" % layout["preview_end"]).encode())
                record("preview")
            elif pc == layout["picker"]:
                output.append(("    if (stronghold_touch_picker()) goto L_%08X;\n" % layout["picker_world"]).encode())
                record("picker")
        # Do not alter the literal disassembly comments. Lazy arithmetic flag
        # lines have no PC marker and belong to the preceding instruction.
        code, marker, comment = line.partition(b"/*")
        if pc==layout["lord_group"] and b"RECOMP_CALL(" in code:
            old=b"RECOMP_CALL(sub_005363F0);" if module=="classic" else b"RECOMP_CALL(sub_00536840);"
            new=b"RECOMP_CALL(sub_005364C0);" if module=="classic" else b"RECOMP_CALL(sub_00536910);"
            assert old in code, "original selection-group callback changed"
            # Native selections queue the original group clear in the same
            # frame; like the desktop double-click, they build the new group
            # unfiltered so stale group marks cannot drop highlighted units.
            code=code.replace(old,b"if (stronghold_touch_group_unfiltered()) { "+new+b" } else { "+old+b" }")
            record("lord_group")
        if layout["tooltip"][0] <= pc < layout["tooltip"][1]:
            token = ("MEM32(0x%X)" % layout["help_flag"]).encode()
            if token in code:
                code = code.replace(token, b"stronghold_touch_ui_help(" + token + b")")
                record("ui_help")
        inspection_button = layout["inspection_buttons"][0] <= pc < layout["inspection_buttons"][1]
        # This separate type-0 callback also closes building inspection on a
        # desktop world Ldown/Rdown. A UI-generated edge must not reach it.
        # Keep its original per-frame building update and all legacy input.
        if (layout["context_range"][0] <= pc < layout["context_range"][1] or
                layout["inspect_range"][0] <= pc < layout["inspect_range"][1] or inspection_button):
            for offset in (0x28, 0x30, 0x34, 0x3C, 0x40, 0x48):
                token = ("MEM32(0x%X)" % (layout["input"] + offset)).encode()
                if token in code:
                    code = code.replace(token, b"stronghold_touch_legacy(" + token + b")")
                    record("inspection_button" if inspection_button else "legacy")
        elif any(lo <= pc < hi for lo, hi in layout["placement_ranges"]):
            for offset, predicate in ((0x28,0), (0x34,1), (0x40,2), (0x30,3), (0x3C,3), (0x48,3)):
                token = ("MEM32(0x%X)" % (layout["input"] + offset)).encode()
                if token in code:
                    code = code.replace(token, ("stronghold_touch_placement(%d, " % predicate).encode() + token + b")")
                    record("placement")
        if layout["renderer"][0] <= pc < layout["renderer"][1]:
            for offset, axis in ((0x10,"x"), (0x14,"y")):
                token = ("MEM32(0x%X)" % (layout["input"] + offset)).encode()
                if token in code:
                    code = code.replace(token, ("stronghold_touch_%s(" % axis).encode() + token + b")")
                    record("world_point")
        if layout["rect"] <= pc < layout["rect"] + 0xB2:
            for axis, offset in (("x", 0x10), ("y", 0x14)):
                token = ("MEM32(ecx + 0x%X)" % offset).encode()
                if token in code:
                    code = code.replace(token, ("stronghold_touch_%s(" % axis).encode() + token + b")")
                    record("pick_point")
        if any(lo <= pc < hi for lo, hi in layout["ui_ranges"]):
            for offset in (0x28,0x30,0x34,0x3C,0x40,0x48):
                token=("MEM32(0x%X)" % (layout["input"]+offset)).encode()
                if token in code:
                    code=code.replace(token,b"stronghold_touch_ui_button("+token+b")")
                    record("ui_button")
            for axis, offset in (("x", 0x10), ("y", 0x14)):
                for token in (("MEM32(0x%X)" % (layout["input"] + offset)).encode(),
                              ("MEM32(ecx + 0x%X)" % offset).encode()):
                    if token in code:
                        code = code.replace(token, ("stronghold_touch_ui_%s(" % axis).encode() + token + b")")
                        record("ui_point")
        output.append(code + marker + comment)
    if not count:
        return text, 0
    result = b"".join(output)
    include = b'#include "recomp_funcs.h"'
    assert include in result
    result = result.replace(include, include + b'\n#include "touch.h"\n#include "display.h"', 1)
    return result, count
