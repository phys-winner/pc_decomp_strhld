package org.stronghold.port;

import java.util.HashSet;
import java.util.HashMap;
import java.util.Map;

public final class ControlLayoutSelfcheck {
    private static void check(boolean value, String message) { if (!value) throw new AssertionError(message); }
    public static void main(String[] args) {
        check(ControlLayout.IDS.length == ControlLayout.LABELS.length, "Every command has a label");
        check(ControlLayout.SCREEN == 0 && ControlLayout.HIDDEN == 2 && ControlLayout.LEFT_ZONE == 3 && ControlLayout.RIGHT_ZONE == 4 && ControlLayout.BOOK_ZONE == 5,
                "Saved placement numbers keep their gap at the removed menu");
        check(!ControlLayout.known("more"), "Removed command menu is absent from the catalog");
        HashSet<String> unique = new HashSet<>();
        for (String id : ControlLayout.IDS) {
            check(unique.add(id), "Unique saved key: " + id);
            check(!ControlLayout.label(id).isEmpty(), "Nonempty label: " + id);
            for (boolean panel : new boolean[]{false, true}) for (boolean cursor : new boolean[]{false, true}) {
                int fallback = ControlLayout.defaultPlacement(id, panel, cursor, false, false);
                check(fallback == ControlLayout.SCREEN || fallback == ControlLayout.HIDDEN || fallback == ControlLayout.BOOK_ZONE, "Valid default: " + id);
                check(ControlLayout.placement(id, -1, fallback) == fallback, "Negative setting: " + id);
                check(ControlLayout.placement(id, 99, fallback) == fallback, "Unknown setting: " + id);
                check(ControlLayout.placement(id, ControlLayout.SCREEN, fallback) == ControlLayout.SCREEN, "Show: " + id);
                check(ControlLayout.placement(id, ControlLayout.HIDDEN, fallback) == ControlLayout.HIDDEN, "Hide: " + id);
                check(ControlLayout.placement(id, 1, fallback) == ControlLayout.HIDDEN, "Legacy menu assignment becomes hidden: " + id);
                for (int zone : new int[]{ControlLayout.LEFT_ZONE, ControlLayout.RIGHT_ZONE, ControlLayout.BOOK_ZONE})
                    check(ControlLayout.placement(id, zone, fallback) ==
                            (ControlLayout.inZone(id) ? zone : fallback), "Command zone: " + id);
                check(fallback != ControlLayout.LEFT_ZONE && fallback != ControlLayout.RIGHT_ZONE, "Zones start empty: " + id);
            }
        }
        for (boolean panel : new boolean[]{false, true}) for (boolean cursor : new boolean[]{false, true})
            check(ControlLayout.defaultPlacement("lmb", panel, cursor, false, false) == ControlLayout.HIDDEN, "No duplicate LMB by default");
        check(ControlLayout.placement("lmb", ControlLayout.SCREEN, ControlLayout.HIDDEN) == ControlLayout.SCREEN, "Manual LMB override");
        for (String id : ControlLayout.IDS) for (boolean cursor : new boolean[]{false, true})
            check(ControlLayout.defaultPlacement(id, false, cursor, false, false) ==
                    ("zoom_value".equals(id) || "gamepad_help".equals(id) || ("rmb".equals(id) && cursor) ? ControlLayout.SCREEN :
                            "menu".equals(id) || "layout".equals(id) ? ControlLayout.BOOK_ZONE : ControlLayout.HIDDEN),
                    "Default controls retain readout, context-sensitive gamepad help and cursor-mode RMB: " + id);
        check(ControlLayout.defaultPlacement("pause", true, false, false, false) == ControlLayout.SCREEN, "Old optional panel remains available");
        check(ControlLayout.defaultPlacement("menu", true, false, false, false) == ControlLayout.SCREEN &&
                ControlLayout.defaultPlacement("layout", true, false, false, false) == ControlLayout.SCREEN, "Old full panel retains menu and layout buttons");
        check(ControlLayout.defaultPlacement("groups", false, false, true, false) == ControlLayout.SCREEN, "Existing group setting");
        check(ControlLayout.defaultPlacement("markers", false, false, false, true) == ControlLayout.SCREEN, "Existing marker setting");
        check(ControlLayout.scan("ctrl") == 0x1d && ControlLayout.scan("shift") == 0x2a, "Modifier ABI");
        check(ControlLayout.scan("f1") == 0x3b && ControlLayout.scan("f5") == 0x3f, "Function key ABI");
        check(ControlLayout.scan("slower") == 0x4a && ControlLayout.scan("faster") == 0x4e, "Numpad speed ABI");
        migrationChecks();
        zoneActionChecks();
        System.out.println("Shared controls: book/defaults, placement ABI, legacy migration, side list/direct actions and hidden LMB PASS (" + unique.size() + " controls)");
    }
    private static void migrationChecks() {
        Map<String, Object> preferences = new HashMap<>();
        check(!ControlLayout.cleanRemovedMenu(preferences), "Empty preferences need no migration");
        check(ControlLayout.placement(preferences, "menu") == ControlLayout.BOOK_ZONE && ControlLayout.placement(preferences, "layout") == ControlLayout.BOOK_ZONE,
                "Fresh settings place layout and pause-menu commands in the scribe book");
        preferences.put("control-menu", 1); preferences.put("control-layout", 1);
        check(ControlLayout.placement(preferences, "menu") == ControlLayout.HIDDEN && ControlLayout.placement(preferences, "layout") == ControlLayout.HIDDEN,
                "Explicit legacy assignments do not fall back to the new book defaults");
        check(ControlLayout.cleanRemovedMenu(preferences) && preferences.get("control-menu").equals(2) && preferences.get("control-layout").equals(2),
                "Book-default commands stay explicitly hidden after legacy migration");
        preferences.clear();
        preferences.put("extra", true); preferences.put("control-pause", 1);
        check(ControlLayout.placement(preferences, "pause") == ControlLayout.HIDDEN, "Legacy1 never falls back to the optional full panel");
        preferences.put("control-relief", 3); preferences.put("control-menu", 0); preferences.put("control-rmb", 4); preferences.put("control-layout", 5);
        preferences.put("control-more", 0); preferences.put("pos-more-x", .2f); preferences.put("pos-more-h", .1f);
        preferences.put("radial-slot-0", "pause"); preferences.put("radial-slot-77", 42);
        preferences.put("pos-pause-x", .3f); preferences.put("groupWheel", true); preferences.put("markerWheel", true);
        preferences.put("control-future", 1); preferences.put("resolution", "600"); preferences.put("radialTap", true);
        check(ControlLayout.cleanRemovedMenu(preferences), "Existing command-menu preferences are migrated");
        check(preferences.get("control-pause").equals(2) && preferences.get("control-relief").equals(3) &&
                preferences.get("control-menu").equals(0) && preferences.get("control-rmb").equals(4) && preferences.get("control-layout").equals(5), "Only obsolete known placements are rewritten");
        check(!preferences.containsKey("control-more") && !preferences.containsKey("pos-more-x") && !preferences.containsKey("pos-more-h") &&
                !preferences.containsKey("radial-slot-0") && !preferences.containsKey("radial-slot-77"), "All obsolete menu keys are removed");
        check(preferences.get("control-future").equals(1) && preferences.get("resolution").equals("600") &&
                preferences.get("pos-pause-x").equals(.3f) && Boolean.TRUE.equals(preferences.get("radialTap")), "Unknown controls and unrelated settings survive migration");
        check(ControlLayout.placement(preferences, "groups") == 0 && ControlLayout.placement(preferences, "markers") == 0,
                "Existing group and bookmark circles survive");
        check(!ControlLayout.cleanRemovedMenu(preferences), "Migration is idempotent");
        preferences.put("control-pause", "wrong type"); preferences.put("extra", "wrong type");
        check(ControlLayout.placement(preferences, "pause") == ControlLayout.HIDDEN, "Wrong preference types recover the safe default");
        preferences.put("control-pause", 99);
        check(ControlLayout.placement(preferences, "pause") == ControlLayout.HIDDEN, "Unknown placement recovers the safe default");
        check(!ControlLayout.inZone("speed") && !ControlLayout.inZone("zoom_value") && !ControlLayout.inZone("more"), "Readouts and removed commands cannot enter side lists");
    }
    private static void zoneActionChecks() {
        Map<String, Object> preferences = new HashMap<>();
        for (int side = 0; side < 2; side++) {
            check(!ControlLayout.zoneDirect(preferences, side), "Side zones default to command lists");
            check(ControlLayout.zoneAction(preferences, side, false).isEmpty() && ControlLayout.zoneAction(preferences, side, true).isEmpty(), "Direct actions default to none");
            check(!ControlLayout.hasZoneActions(preferences, side), "Empty direct bindings cannot activate a zone");
        }
        preferences.put("zone-left-mode", "actions"); preferences.put("zone-left-tap", "pause"); preferences.put("zone-left-hold", "layout");
        check(ControlLayout.zoneDirect(preferences, 0) && ControlLayout.hasZoneActions(preferences, 0), "Configured direct mode activates the left zone");
        check(ControlLayout.zoneAction(preferences, 0, false).equals("pause") && ControlLayout.zoneAction(preferences, 0, true).equals("layout"), "Tap and hold bindings are independent");
        check(!ControlLayout.zoneDirect(preferences, 1) && ControlLayout.zoneAction(preferences, 1, false).isEmpty(), "The right zone remains independent");
        preferences.put("zone-left-mode", "list");
        check(!ControlLayout.zoneDirect(preferences, 0) && !ControlLayout.hasZoneActions(preferences, 0) && ControlLayout.zoneAction(preferences, 0, false).equals("pause"),
                "Switching back to a list preserves bindings but disables direct execution");
        for (Object invalid : new Object[]{true, 5, "unknown", ""}) {
            preferences.put("zone-left-mode", invalid);
            check(!ControlLayout.zoneDirect(preferences, 0), "Invalid mode falls back to list");
        }
        preferences.put("zone-left-mode", "actions");
        for (Object invalid : new Object[]{true, 5, "unknown", "more", "speed", "zoom_value", ""}) {
            preferences.put("zone-left-tap", invalid); preferences.put("zone-left-hold", invalid);
            check(ControlLayout.zoneAction(preferences, 0, false).isEmpty() && ControlLayout.zoneAction(preferences, 0, true).isEmpty() && !ControlLayout.hasZoneActions(preferences, 0),
                    "Invalid/empty actions do not create an active zone");
        }
        preferences.put("zone-left-hold", "menu");
        check(ControlLayout.hasZoneActions(preferences, 0), "Hold-only binding is sufficient");
        preferences.put("zone-right-mode", "actions"); preferences.put("zone-right-tap", "rmb");
        check(ControlLayout.hasZoneActions(preferences, 1) && ControlLayout.zoneAction(preferences, 1, false).equals("rmb"), "Right action mode can select any zone command");
        check(!ControlLayout.cleanRemovedMenu(preferences), "Menu cleanup leaves direct-zone preferences unchanged");
        for (int invalid : new int[]{-1, 2, Integer.MAX_VALUE})
            check(!ControlLayout.zoneDirect(preferences, invalid) && ControlLayout.zoneAction(preferences, invalid, true).isEmpty() && !ControlLayout.hasZoneActions(preferences, invalid),
                    "Only the two side zones support direct mode; the book stays a list");
    }
}
