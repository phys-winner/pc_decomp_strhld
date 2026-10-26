"""Hash-pinned seams of the original Stronghold HD (Stronghold.exe 8b581f48...).

mode: WinMain (532980) loads stronghold.cfg (47EB40), creates the window
(45EB50) and then applies the saved display mode: 532AAD reads the screen
object's mode index (00B182C8+5C = 00B18324) and passes it to 461540, which
selects the size from the engine's own table (45E4A0: 1 800x600, 2 1024x768,
3 1280x720, ... 10 1920x1080, 15 1024x600) and creates the DirectDraw
surfaces. stronghold_original_mode() may replace a missing or unsupported
saved index with the Android request; the engine itself validates it against
the enumerated modes (458CC0) and computes all geometry as on Windows.

frame: once per active frame WinMain updates the original scroll state
(532EBA: mov ecx, 0BCAF70h; call 4599E0). Touch camera moves are applied just
before it on the game thread, the way the original scroll code moves the view
(42D4B8..42D50F: camera 016E9358 +74/+78, then 4B33B0 clamps it).

Wide frames keep a table mode of the requested height and replace its width.
Everything vertical follows the mode index; the width-specific code is:
  dimensions  45E5F4, where 45E4A0 derives pitch, buffers and the centered
              800x600 origin from the stored size;
  view        4B33B0 (camera clamp, tail of the per-mode view table 4B3EC0):
              visible tile columns +84 and the rotation pivot +A0;
  edges       42A000, 42AE60, 431FD0 load the HUD side pair of the mode's
              width ("edge1280l.tgx" ...) and draw it with the unclipped TGX
              blit (447DB0 -> 4408D0); a wider stock pair goes through the
              clipped one (448E60 -> 4480C0, same arguments and cleanup);
  bars        4919E0 / 491BE0 subtract the mode's edge width from x;
  fill        4A73E0 fills the mode's rectangle.

UI layers: in a match, WinMain renders the world (532F56: 4B5110), then the
anchored overlays (490160 calls each item's draw at 490221: resources at the
top left, hints above the panel), the drag box and markers, the in-game
dialogs (4912F0 for 01503CB8 at 533122 and 018ECCE8 at 53312C, all into the
world raster), copies the keyed HUD overhang into the world raster (533141:
448560) and presents (533156: 45EC40). The UI tree passes (4BDBE0 at 533104
and 533118) draw the panel into the UI surface, but labels above it (the
hovered building's name) into the world raster. The overlays, those passes
and the overhang draw into layers instead; dialogs are lifted out of the
raster after they are drawn. The opaque raster families (TGX 4408D0, 4480C0, GM1 4430F0, keyed
copy 448560, row/column fills 459D00/459D60) report their stores.

Translucent primitives report how much of the pixel below stays visible
(stronghold_display_blended), so a layer pre-filled with the transparent
colour composes exactly over the world: the shaded rectangle 442520 keeps
factor/32 (table 9CE0C8 + 512 * factor in esi at its stores), the halving
row 459DC0 keeps one half, and the masked sprite 442670 (frames of the boxes
460160 draws behind banners such as "Победа") keeps (32 - a)/32 for the mask
level a = (mask & 31) - fade, the mask read just before each store. Its 555
path sums source and destination in two stores; only the second reports.
"""
import re
import display_hooks

seen = set()
EDGE_NAMES = (0x55804C, 0x55805C, 0x55806C, 0x55807C, 0x55808C, 0x55809C, 0x5580AC, 0x5580BC, 0x5580CC,
              0x5580DC, 0x5580EC, 0x5580FC, 0x55810C, 0x55811C, 0x55812C, 0x55813C, 0x55814C, 0x55815C,
              0x55816C, 0x558184, 0x55819C, 0x5581B4, 0x5581CC, 0x5581E4, 0x5581FC, 0x558214, 0x55822C,
              0x558244, 0x55825C, 0x558274, 0x55828C, 0x5582A4, 0x5582BC, 0x5582D4, 0x5582EC, 0x558304)
LAYOUT = {
    "stronghold": dict(
        mode=(0x532AAD, 0xB18324), frame=(0x532EBA, 0xBCAF70),
        dimensions=0x45E5F4, view=0x4B33B0, bars=(0x4919E0, 0x491BE0), fill=((0x4A73E0, 0x4A7600), 0x45FCB0),
        edges=(((0x42A000, 0x42A6D5), (0x42AE60, 0x42C450), (0x431FD0, 0x432E53)), 0x447DB0, 0x448E60),
        layers=(0x532F56, 0x4B5110), item=(0x490221, 0x490223), items_end=(0x532F60, b"eax = MEM32(0xABC37C); "),
        boxes=((0x533122, 0x4912F0, 0x533127, 0x1503CB8), (0x53312C, 0x4912F0, 0x533131, 0x18ECCE8)),
        panel=((0x533104, 0x4BDBE0, 0x533109), (0x533118, 0x4BDBE0, 0x53311D), (0x533141, 0x448560, 0x533146)),
        present=(0x533156, 0x45EC40),
        raster=((0x4408D0, 0x441A06), (0x4430F0, 0x4452CF), (0x4480C0, 0x448550), (0x448560, 0x4486AC),
                (0x459D00, 0x459DBF), (0x442520, 0x44266C), (0x442670, 0x442BDE), (0x459DC0, 0x459E3B)),
    ),
}
# Remaining weight of the pixel below at a translucent store (0..255); None: an intermediate store.
SHADE = b"(((esi - 0x9CE0C8u) >> 9) * 255u / 32u)"
MASKED = b"((32u - ((MEM16(MEM32(ebp - 4u) - 2u) & 31u) - MEM32(ebp + 0x1Cu))) * 255u / 32u)"
BLEND = {0x4425D3: SHADE, 0x44264B: SHADE, 0x4428AC: MASKED, 0x442995: MASKED, 0x442B05: MASKED,
         0x442BBE: MASKED, 0x459E2A: b"128u", 0x44293F: None}
DECLARATIONS = (b"\nextern uint32_t stronghold_original_mode(uint32_t);"
                b"\nextern void stronghold_original_frame(void);"
                b"\nextern void stronghold_game_ui_click(uint32_t);"
                b"\nextern void stronghold_text_focus(uint32_t);"
                b"\nextern void stronghold_original_build_click(uint32_t);"
                b"\nextern void stronghold_original_build_draw(uint32_t);"
                b"\nextern int stronghold_touch_ui_redraw(void);"
                b"\nextern uint32_t stronghold_touch_ui_help(uint32_t);"
                b"\nextern uint32_t stronghold_original_radar_color(uint32_t, uint32_t);"
                b"\nextern void stronghold_original_dimensions(uint32_t, uint32_t);"
                b"\nextern void stronghold_original_view(uint32_t);"
                b"\nextern void stronghold_original_bar(uint32_t);"
                b"\nextern void stronghold_original_progress(int);"
                b"\nextern void stronghold_original_fill(uint32_t);"
                b"\nextern int stronghold_original_edge(uint32_t);"
                b"\nextern uint32_t stronghold_original_edge_name(uint32_t);"
                b"\nextern void stronghold_original_layers(void);"
                b"\nextern void stronghold_original_resource_animation(int);"
                b"\nextern void stronghold_original_item(uint32_t, int);"
                b"\nextern void stronghold_original_items_end(void);"
                b"\nextern void stronghold_original_freeze(void);"
                b"\nextern void stronghold_original_box(uint32_t);"
                b"\nextern void stronghold_original_panel(int);"
                b"\nextern void stronghold_original_present(void);"
                b"\nextern int stronghold_display_capture_active;"
                b"\nextern void stronghold_display_written(uint32_t, unsigned);"
                b"\nextern void stronghold_display_blended(uint32_t, unsigned, unsigned);")
PC = re.compile(rb"/\* 0x([0-9A-F]{8}):")


def required(module):
    if module not in LAYOUT:
        return set()
    keys = {"mode", "frame", "dimensions", "view", "bar004919E0", "bar00491BE0", "fill", "edges", "edge_names",
            "layers", "item", "item_end", "items_end", "freeze", "box01503CB8", "box018ECCE8", "present", "raster", "blends",
            "editor_focus", "editor_click004BC0D5", "editor_click004BC160",
            "build_draw", "build_redraw", "build_help", "radar0049574A", "radar0049576E",
            "save004396E3", "save0047E0AB", "save0047CCD8", "cart", "cart_end", "opaque-batching"}
    keys |= {f"blend{pc:08X}" for pc in BLEND}
    keys |= {f"{kind}{pc:08X}" for pc, _, _ in LAYOUT[module]["panel"] for kind in ("panel", "panel_end")}
    return {f"{module}:{key}" for key in keys}


def apply(text, module):
    layout = LAYOUT.get(module)
    if layout is None:
        return text, 0
    total = 0

    def note(key, count):
        nonlocal total
        if count:
            seen.add(f"{module}:{key}")
            total += count

    def before_line(pc, code, key, expect=rb"[^\r\n]*?"):
        """Inserts code at the start of the instruction line of pc."""
        nonlocal text
        if b"/* 0x%08X:" % pc not in text:
            return
        pattern = rb"^(    )(%s/\* 0x%08X: [^\r\n]*\*/\r?)$" % (expect, pc)
        text, count = re.subn(pattern, lambda m: m[1] + code + b" " + m[2], text, flags=re.M)
        if not count:
            raise ValueError(f"{module}:{pc:08X}: original instruction changed")
        note(key, count)

    def call(pc, callee):
        return rb"RECOMP_CALL\(sub_%08X\); " % callee

    pc, index = layout["mode"]
    if b"/* 0x%08X:" % pc in text:
        pattern = (rb"^(    )edx = MEM32\(0x%X\);( /\* 0x%08X: mov edx, dword ptr \[0x%x\] \*/)(\r?)$"
                   % (index, pc, index))
        replacement = b"MEM32(0x%Xu) = stronghold_original_mode(MEM32(0x%X)); edx = MEM32(0x%X);" % (index, index, index)
        text, count = re.subn(pattern, lambda m: m[1] + replacement + m[2] + m[3], text, flags=re.M)
        if not count:
            raise ValueError(f"{module}:{pc:08X}: original display-mode load changed")
        note("mode", count)
    pc, scroll = layout["frame"]
    if b"/* 0x%08X:" % pc in text:
        before_line(pc, b"RECOMP_REGS_OUT(); stronghold_original_frame(); RECOMP_REGS_IN();", "frame",
                    rb"ecx = 0x%08Xu; " % scroll)
    # Original accepted bank focus (after lock/range checks), and the actual
    # widget press/repeat callbacks. A cancelled Android editor can be reopened
    # by tapping the same focused field without treating unrelated UI as text.
    before_line(0x45A780,b"stronghold_text_focus(ecx);","editor_focus",rb"MEM32\(ecx\) = eax; ")
    for pc in (0x4BC0D5,0x4BC160):
        before_line(pc,b"stronghold_game_ui_click(esi); stronghold_original_build_click(esi);",f"editor_click{pc:08X}",rb"RECOMP_ICALL\(edx\); ")
    before_line(0x4BC603,b"stronghold_original_build_draw(ecx);","build_draw",rb"esi = ecx; ")
    before_line(0x532FA6,b"if (stronghold_touch_ui_redraw()) goto L_005330D3;","build_redraw",rb"if \(CMP_EQ\(_flag_a, _flag_b\)\) goto L_005330D3; ")
    # Only the original construction tooltip's help preference is replaced.
    # All localized labels, prices, affordability and draw callbacks stay original.
    lines=[];current_pc=0
    for line in text.splitlines(keepends=True):
        match=PC.search(line)
        if match:current_pc=int(match[1],16)
        code,marker,comment=line.partition(b"/*")
        if 0x4BDDF0<=current_pc<0x4BE286 and b"MEM32(0x1503DE8)" in code:
            code=code.replace(b"MEM32(0x1503DE8)",b"stronghold_touch_ui_help(MEM32(0x1503DE8))")
            note("build_help",1)
        lines.append(code+marker+comment)
    text=b"".join(lines)
    for pc,register in ((0x49574A,b"eax"),(0x49576E,b"ecx")):
        if b"/* 0x%08X:"%pc not in text:continue
        pattern=rb"(?m)^(    )"+register+rb" = MEM32\(ecx \* 4 \+ 0x7A6924\);( /\* 0x%08X:[^\r\n]*\*/\r?)$"%pc
        text,count=re.subn(pattern,lambda m:m[1]+register+b" = stronghold_original_radar_color(ecx, MEM32(ecx * 4 + 0x7A6924));"+m[2],text)
        if not count:raise ValueError(f"{module}:{pc:08X}: original radar owner-palette lookup changed")
        note(f"radar{pc:08X}",count)

    # 45E4A0 stores the mode's size, then derives everything else at 45E5F4 (ecx screen, ebx mode).
    label = b"L_%08X:" % layout["dimensions"]
    if label in text:
        text, count = re.subn(rb"^" + label + rb"(\r?)\n", lambda m: label + m[1] + b"\n    stronghold_original_dimensions(ecx, ebx);\n",
                              text, flags=re.M)
        note("dimensions", count)
    entry = b"RECOMP_ENTER(0x%08Xu);" % layout["view"]
    if entry in text:
        count = text.count(entry)
        text = text.replace(entry, entry + b" stronghold_original_view(ecx);")
        note("view", count)
    for function in layout["bars"]:
        entry = b"RECOMP_ENTER(0x%08Xu);" % function
        if entry in text:
            count = text.count(entry)
            text = text.replace(entry, entry + b" stronghold_original_bar(esp);")
            note("bar%08X" % function, count)

    def per_line(ranges, edit):
        """Applies edit(line, pc) to every instruction line whose address lies in ranges."""
        nonlocal text
        blocks = sorted({b"%06X" % (a >> 8) for lo, hi in ranges for a in range(lo & ~0xFF, hi, 0x100)})
        if not any(b"/* 0x" + block in text for block in blocks):
            return
        pattern = rb"(?m)^    [^\r\n]*/\* 0x(?:%s)[0-9A-F]{2}:[^\r\n]*\*/\r?$" % b"|".join(blocks)
        def line(m):
            pc = int(PC.search(m[0])[1], 16)
            return edit(m[0], pc) if any(lo <= pc < hi for lo, hi in ranges) else m[0]
        text = re.sub(pattern, line, text)

    (fill_range, fill_callee) = layout["fill"]
    fills = 0
    def fill(line, pc):
        nonlocal fills
        marker = b"RECOMP_CALL(sub_%08X);" % fill_callee
        if marker not in line:
            return line
        fills += 1
        return line.replace(marker, b"stronghold_original_fill(esp); " + marker, 1)
    per_line((fill_range,), fill)
    note("fill", fills)

    edge_ranges, draw, clipped = layout["edges"]
    edges = names = 0
    def edge(line, pc):
        nonlocal edges, names
        marker = b"RECOMP_CALL(sub_%08X);" % draw
        if marker in line:
            edges += 1
            line = line.replace(marker, b"{ if (stronghold_original_edge(esp)) RECOMP_CALL(sub_%08X); else %s }" % (clipped, marker), 1)
        push = re.match(rb"^(    )PUSH32\(esp, 0x0*([0-9A-F]+)u\);", line)
        if push and int(push[2], 16) in EDGE_NAMES:
            names += 1
            line = re.sub(rb"PUSH32\(esp, (0x0*[0-9A-F]+u)\);", rb"PUSH32(esp, stronghold_original_edge_name(\1));", line, count=1)
        return line
    per_line(edge_ranges, edge)
    note("edges", edges)
    note("edge_names", names)

    pc, callee = layout["layers"]
    for save,bar in ((0x4396E3,0),(0x47E0AB,0),(0x47CCD8,1)):
        before_line(save,b"stronghold_original_progress(%d);" % bar,f"save{save:08X}",call(save,0x45EC40))
    before_line(pc, b"stronghold_original_layers();", "layers", call(pc, callee))
    before_line(0x42D2DF, b"stronghold_original_resource_animation(1);", "cart", call(0x42D2DF, 0x448880))
    before_line(0x42D2E4, b"stronghold_original_resource_animation(0);", "cart_end", rb"goto L_0042D310; ")
    pc, after = layout["item"]
    before_line(pc, b"stronghold_original_item(esi, 1);", "item", rb"RECOMP_ICALL\(ecx\); ")
    before_line(after, b"stronghold_original_item(esi, 0);", "item_end")
    pc, expect = layout["items_end"]
    before_line(pc, b"stronghold_original_items_end();", "items_end", re.escape(expect))
    for index, (pc, callee, after, box) in enumerate(layout["boxes"]):
        if not index:
            before_line(pc, b"stronghold_original_freeze();", "freeze", call(pc, callee))
        before_line(after, b"stronghold_original_box(0x%08Xu);" % box, "box%08X" % box)
    for pc, callee, after in layout["panel"]:
        before_line(pc, b"stronghold_original_panel(1);", "panel%08X" % pc, call(pc, callee))
        before_line(after, b"stronghold_original_panel(0);", "panel_end%08X" % pc)
    pc, callee = layout["present"]
    before_line(pc, b"stronghold_original_present();", "present", call(pc, callee))

    # Raster stores of the UI families report to the open layer: opaque, or
    # translucent with the weight left to the pixel below (BLEND).
    stores = 0
    blends = set()
    def raster(line, pc):
        nonlocal stores
        if display_hooks.raster_stack_store(line,pc,module):return line
        if pc in BLEND and BLEND[pc] is None:
            blends.add(pc)
            return line
        def store(m):
            nonlocal stores
            stores += 1
            if pc in BLEND:
                blends.add(pc)
                return m[0] + b" if (stronghold_display_capture_active) stronghold_display_blended(%s, %d, %s);" % (
                    m[2], int(m[1]) // 8, BLEND[pc])
            return m[0] + b" if (stronghold_display_capture_active) stronghold_display_written(%s, %d);" % (m[2], int(m[1]) // 8)
        return re.sub(rb"MEM(16|32)\(([^;]+?)\) = [^;]+;", store, line)
    per_line(layout["raster"], raster)
    note("raster", stores)
    # HD's capture calls are inserted here, after display_hooks' shared half-size loops.
    text, spans = display_hooks.batch_opaque_runs(text,module)
    text, edges = display_hooks.batch_opaque_backedges(text,module)
    if spans != edges:raise ValueError(f'{module}: opaque copy/capture batching mismatch')
    note("opaque-batching", spans + edges)
    if blends:
        note("blends", len(blends))
        seen.update(f"{module}:blend{pc:08X}" for pc in blends)

    if total:
        include = b'#include "recomp_funcs.h"'
        if include not in text:
            raise ValueError(f"{module}: generated source lacks its type header")
        text = text.replace(include, include + DECLARATIONS, 1)
    return text, total


if __name__ == "__main__":
    from pathlib import Path
    root = Path(__file__).resolve().parents[1]
    for module in LAYOUT:
        seen.clear()
        total = 0
        for path in (root / "generated" / f"{module}-x86").glob("recomp_*.c"):
            text, count = apply(path.read_bytes(), module)
            total += count
        assert not (required(module) - seen), sorted(required(module) - seen)
        print(f"{module}: {len(seen)} seams in {total} lifted places PASS")
    for module in ("game", "classic", "mss", "bink"):
        assert apply(b'#include "recomp_funcs.h"\n', module) == (b'#include "recomp_funcs.h"\n', 0)
    print("Crusader and DLL sources unchanged: PASS")
