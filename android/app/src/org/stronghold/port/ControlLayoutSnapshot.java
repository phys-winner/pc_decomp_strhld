package org.stronghold.port;

import java.util.LinkedHashMap;
import java.util.LinkedHashSet;
import java.util.Map;
import java.util.Set;

/** Cancel restores control choices without rolling back unrelated game settings. */
final class ControlLayoutSnapshot {
    static boolean isLayoutKey(String key) {
        return key != null && !ControlLayout.removedMenuKey(key) && (key.startsWith("pos-") || key.startsWith("control-") || "extra".equals(key) ||
                "groupWheel".equals(key) || "markerWheel".equals(key) ||
                "zone-left-mode".equals(key) || "zone-left-tap".equals(key) || "zone-left-hold".equals(key) ||
                "zone-right-mode".equals(key) || "zone-right-tap".equals(key) || "zone-right-hold".equals(key));
    }

    static Map<String, Object> capture(Map<String, ?> preferences) {
        Map<String, Object> saved = new LinkedHashMap<>();
        for (Map.Entry<String, ?> entry : preferences.entrySet()) if (isLayoutKey(entry.getKey())) {
            Object value = entry.getValue();
            saved.put(entry.getKey(), value instanceof Set ? new LinkedHashSet<>((Set<?>)value) : value);
        }
        ControlLayout.cleanRemovedMenu(saved);
        return saved;
    }
}
