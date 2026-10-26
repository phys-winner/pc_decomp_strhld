package org.stronghold.port;

import java.util.Map;

/** Shared command placements; saved positions use fractions of the whole game view. */
final class ControlLayout {
    static final int SCREEN = 0, HIDDEN = 2, LEFT_ZONE = 3, RIGHT_ZONE = 4, BOOK_ZONE = 5;
    static final String[] IDS = {"menu", "pause", "text", "settings", "layout", "help", "switch",
            "ctrl", "shift", "backspace", "enter", "f5", "f1", "slower", "speed", "faster",
            "relief", "zoom", "rotate_left", "rotate_right", "editor", "lmb", "rmb", "groups", "markers", "zoom_value",
            "editor_pause", "editor_resources", "editor_lord", "editor_type", "editor_animals", "editor_seams", "gamepad_help"};
    static final String[] LABELS = {"Меню", "Пауза", "Ввод", "Настройки", "Расположение", "?", "Другая игра / лаунчер",
            "Ctrl", "Shift", "⌫", "Enter", "F5", "F1 · замок", "−", "Скорость игры", "+",
            "Рельеф", "Масштаб", "↺", "↻", "Редактор…", "ЛКМ", "ПКМ", "Отряд", "Точки", "Приближение карты",
            "Редактор: пауза", "Редактор: налоги, деньги и рацион", "Редактор: восстановить лорда",
            "Редактор: тип карты", "Редактор: удалить животных", "Редактор: швы местности", "Геймпад · раскладка"};
    static String label(String id) {
        for (int i = 0; i < IDS.length; i++) if (IDS[i].equals(id)) return LABELS[i];
        throw new IllegalArgumentException(id);
    }
    static boolean known(String id) {
        for (String candidate : IDS) if (candidate.equals(id)) return true;
        return false;
    }
    static boolean inZone(String id) { return known(id) && !"speed".equals(id) && !"zoom_value".equals(id); }
    static int defaultPlacement(String id, boolean extra, boolean touchpad, boolean groups, boolean markers) {
        if (!known(id)) return HIDDEN;
        if ("zoom_value".equals(id) || "gamepad_help".equals(id)) return SCREEN;
        // Screen taps already left-click in cursor mode; an explicit choice can restore the button.
        if ("lmb".equals(id)) return HIDDEN;
        if ("rmb".equals(id)) return touchpad ? SCREEN : HIDDEN;
        if ("groups".equals(id)) return groups ? SCREEN : HIDDEN;
        if ("markers".equals(id)) return markers ? SCREEN : HIDDEN;
        if (id.startsWith("editor_")) return HIDDEN;
        if (!extra && ("layout".equals(id) || "menu".equals(id))) return BOOK_ZONE;
        return extra ? SCREEN : HIDDEN;
    }
    static int placement(String id, int saved, int fallback) {
        if (!known(id)) return HIDDEN;
        if (saved == 1) return HIDDEN; // Legacy command-menu placement must not revive a menu after updating.
        if (saved == SCREEN || saved == HIDDEN) return saved;
        return (saved == LEFT_ZONE || saved == RIGHT_ZONE || saved == BOOK_ZONE) && inZone(id) ? saved : fallback;
    }
    private static boolean flag(Map<String, ?> preferences, String key) { return Boolean.TRUE.equals(preferences.get(key)); }
    static int placement(Map<String, ?> preferences, String id) {
        int fallback = defaultPlacement(id, flag(preferences, "extra"), flag(preferences, "touchpad"),
                flag(preferences, "groupWheel"), flag(preferences, "markerWheel"));
        Object saved = preferences.get("control-" + id);
        return placement(id, saved instanceof Integer ? (Integer)saved : fallback, fallback);
    }
    private static String zonePrefix(int side) { return side == 0 ? "zone-left-" : side == 1 ? "zone-right-" : null; }
    static boolean zoneDirect(Map<String, ?> preferences, int side) {
        String prefix = zonePrefix(side);
        return prefix != null && "actions".equals(preferences.get(prefix + "mode"));
    }
    static String zoneAction(Map<String, ?> preferences, int side, boolean hold) {
        String prefix = zonePrefix(side);
        Object value = prefix == null ? null : preferences.get(prefix + (hold ? "hold" : "tap"));
        return value instanceof String && inZone((String)value) ? (String)value : "";
    }
    static boolean hasZoneActions(Map<String, ?> preferences, int side) {
        return zoneDirect(preferences, side) && (!zoneAction(preferences, side, false).isEmpty() || !zoneAction(preferences, side, true).isEmpty());
    }
    static boolean removedMenuKey(String key) {
        return key != null && (key.startsWith("radial-slot-") || "control-more".equals(key) ||
                "pos-more".equals(key) || key.startsWith("pos-more-"));
    }
    static boolean cleanRemovedMenu(Map<String, Object> preferences) {
        boolean changed = false;
        java.util.Iterator<String> keys = preferences.keySet().iterator();
        while (keys.hasNext()) if (removedMenuKey(keys.next())) { keys.remove(); changed = true; }
        for (String id : IDS) {
            String key = "control-" + id;
            if (Integer.valueOf(1).equals(preferences.get(key))) { preferences.put(key, HIDDEN); changed = true; }
        }
        return changed;
    }
    static int scan(String id) {
        switch (id) {
            case "ctrl": return 0x1d;
            case "shift": return 0x2a;
            case "backspace": return 0x0e;
            case "enter": return 0x1c;
            case "f5": return 0x3f;
            case "f1": return 0x3b;
            case "slower": return 0x4a;
            case "faster": return 0x4e;
            default: return 0;
        }
    }
}
