package org.stronghold.port;

import java.util.Arrays;
import java.util.LinkedHashMap;
import java.util.LinkedHashSet;
import java.util.Map;
import java.util.Set;

public final class ControlLayoutSnapshotSelfcheck {
    private static void check(boolean value, String message) { if (!value) throw new AssertionError(message); }
    public static void main(String[] args) {
        Map<String, Object> preferences = new LinkedHashMap<>();
        preferences.put("pos-pause-x", .25f); preferences.put("pos-pause-alpha", .7f);
        preferences.put("control-pause", 3); preferences.put("pos-zone_left-w", .12f);
        preferences.put("control-layout", 5);
        preferences.put("zone-left-mode", "actions"); preferences.put("zone-left-tap", "pause"); preferences.put("zone-left-hold", "layout");
        preferences.put("zone-right-mode", "list"); preferences.put("zone-right-tap", ""); preferences.put("zone-right-hold", "menu");
        preferences.put("zone-left-other", "unrelated"); preferences.put("zone-right-more-tap", "unrelated");
        preferences.put("radial-slot-7", "layout"); preferences.put("control-more", 0); preferences.put("pos-more-x", .5f);
        preferences.put("extra", true); preferences.put("groupWheel", false); preferences.put("markerWheel", true);
        preferences.put("resolution", "600"); preferences.put("smoothCamera", true); preferences.put("uiScale", 120);
        Set<String> set = new LinkedHashSet<>(Arrays.asList("pause", "relief"));
        preferences.put("control-test-set", set);
        Map<String, Object> saved = ControlLayoutSnapshot.capture(preferences);
        check(saved.size() == 15, "Only layout keys, the book placement and the six side-action keys are captured");
        check(!saved.containsKey("resolution") && !saved.containsKey("smoothCamera") && !saved.containsKey("uiScale"),
                "Game preferences are outside the layout transaction");
        check(!saved.containsKey("radial-slot-7") && !saved.containsKey("control-more") && !saved.containsKey("pos-more-x"),
                "Removed command-menu keys cannot return through Save or Cancel");
        check(!saved.containsKey("zone-left-other") && !saved.containsKey("zone-right-more-tap"), "Only the exact six side-action settings belong to layout state");
        check(!ControlLayoutSnapshot.isLayoutKey(null) && !ControlLayoutSnapshot.isLayoutKey("position") &&
                !ControlLayoutSnapshot.isLayoutKey("controlState") && !ControlLayoutSnapshot.isLayoutKey("radialTap"),
                "Similar key names are not layout state");
        preferences.put("pos-pause-x", .8f); preferences.put("control-pause", 2);
        preferences.put("pos-new-x", .5f); preferences.remove("pos-zone_left-w");
        preferences.put("zone-left-mode", "list"); preferences.put("zone-left-tap", "rmb"); preferences.remove("zone-right-hold");
        preferences.put("resolution", "720"); preferences.put("smoothCamera", false);
        set.clear();
        check(((Set<?>)saved.get("control-test-set")).size() == 2, "Captured sets do not alias mutable source values");
        check(saved.get("pos-pause-x").equals(.25f), "Changed layout values do not alter the snapshot");
        for (String key : new LinkedHashSet<>(preferences.keySet()))
            if (ControlLayoutSnapshot.isLayoutKey(key)) preferences.remove(key);
        preferences.putAll(saved);
        check(preferences.get("pos-pause-x").equals(.25f) && preferences.get("control-pause").equals(3),
                "Cancel restores original geometry and placement");
        check(!preferences.containsKey("pos-new-x") && preferences.get("pos-zone_left-w").equals(.12f),
                "Cancel removes new keys and restores removed zone geometry");
        check(preferences.get("zone-left-mode").equals("actions") && preferences.get("zone-left-tap").equals("pause") && preferences.get("zone-right-hold").equals("menu") && preferences.get("control-layout").equals(5),
                "Cancel restores side modes/bindings and the book placement");
        check(preferences.get("resolution").equals("720") && preferences.get("smoothCamera").equals(false),
                "Cancel does not roll back unrelated changes made during editing");
        check(ControlLayoutSnapshot.capture(new LinkedHashMap<String, Object>()).isEmpty(), "An empty snapshot is valid");
        Map<String, Object> legacy = new LinkedHashMap<>(); legacy.put("control-pause", 1);
        Map<String, Object> migrated = ControlLayoutSnapshot.capture(legacy);
        check(migrated.get("control-pause").equals(2) && legacy.get("control-pause").equals(1),
                "Snapshot normalizes legacy assignments without mutating the source");
        System.out.println("Control layout snapshots: filtered keys, independent copy and unrelated preference preservation PASS");
    }
}
