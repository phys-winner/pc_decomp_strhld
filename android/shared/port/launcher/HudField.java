package port.launcher;

import android.content.Context;
import java.util.Properties;

/** Append fields at the end: their bits are saved in the player's HUD settings. */
enum HudField {
    GRAPH("FPS graph", "График FPS", "FPS-Diagramm", "Wykres FPS"),
    RANGE("Min / max frame time (ms)", "Мин. / макс. время кадра (мс)", "Min. / max. Bildzeit (ms)", "Min. / maks. czas klatki (ms)"),
    CPU("Processor", "Процессор", "Prozessor", "Procesor"),
    CLOCK("CPU frequencies", "Частоты процессора", "CPU-Takt", "Taktowanie CPU"),
    LOAD("CPU load and busiest threads", "Загрузка CPU и активные потоки", "CPU-Last und aktive Threads", "Obciążenie CPU i aktywne wątki"),
    CPU_TEMP("CPU temperature", "Температура CPU", "CPU-Temperatur", "Temperatura CPU"),
    GPU("Graphics processor", "Графический процессор", "Grafikprozessor", "Procesor graficzny"),
    DRIVER("Graphics driver", "Драйвер графики", "Grafiktreiber", "Sterownik graficzny"),
    API("Renderer and API version", "Рендерер и версия API", "Renderer und API-Version", "Renderer i wersja API"),
    GPU_TEMP("GPU temperature", "Температура GPU", "GPU-Temperatur", "Temperatura GPU"),
    RAM("Memory usage", "Использование памяти", "Speicherbelegung", "Użycie pamięci"),
    THERMAL("Thermal status and game mode", "Нагрев и игровой режим", "Thermostatus und Spielmodus", "Stan termiczny i tryb gry"),
    BATTERY("Battery level and temperature", "Заряд и температура батареи", "Akkustand und Temperatur", "Poziom i temperatura baterii"),
    POWER("Battery power (W)", "Мощность батареи (Вт)", "Akkuleistung (W)", "Moc baterii (W)"),
    SCREEN("Display, refresh rate and FPS limit", "Экран, частота и лимит FPS", "Display, Frequenz und FPS-Limit", "Ekran, odświeżanie i limit FPS");

    static final String KEY = "hud.rows";
    static final int FRAMES = GRAPH.bit | RANGE.bit;
    static final int ALL = (1 << values().length) - 1;
    final int bit = 1 << ordinal();
    final String[] title;

    HudField(String... title) { this.title = title; }

    boolean in(int mask) { return (mask & bit) != 0; }

    static int mask(Properties prefs, String mode) {
        int legacy = "2".equals(mode) ? ALL : "1".equals(prefs.getProperty("option.fps_counter")) ? FRAMES : 0;
        return (int)State.getLong(prefs, KEY, legacy) & ALL;
    }

    static int read(Context context, String mode) { return mask(State.read(context, State.CHOICES), mode); }
}
