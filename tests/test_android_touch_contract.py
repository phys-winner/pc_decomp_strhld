"""Cross-language touch ABI and Android process isolation, without building an APK."""
from pathlib import Path
import re
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
JAVA = ROOT / "android/app/src/org/stronghold/port"
ANDROID = "{http://schemas.android.com/apk/res/android}"


def main():
    native = dict((name, int(value)) for name, value in re.findall(
        r"SH_TOUCH_(\w+)\s*=\s*(\d+)", (ROOT / "android/host/touch_api.h").read_text()))
    source = (JAVA / "SemanticTouchControls.java").read_text(encoding="utf-8")
    declarations = re.findall(r"static final int ([^;]+);", source)
    java = dict((name, int(value)) for block in declarations for name, value in
                re.findall(r"(\w+)\s*=\s*(\d+)", block))
    assert {name: value for name, value in java.items() if name in native} == native, (java, native)
    assert "nativeMouse" not in source, "Semantic gestures must not emit a mouse stream"
    # Every native method of the common base class is exported by the JNI glue (no runtime UnsatisfiedLinkError).
    activity = (JAVA / "GameActivity.java").read_text(encoding="utf-8")
    settings_events = dict((name, int(value)) for name, value in re.findall(
        r"SH_(UI_SETTINGS_\w+)\s*=\s*(\d+)", (ROOT / "android/host/game_ui.h").read_text()))
    for name, value in settings_events.items():
        assert re.search(rf"\b{name}\s*=\s*{value}\b", activity), name
    declared = set(re.findall(r"\bnative\s+[\w\[\]]+\s+(\w+)\s*\(", activity))
    glue = (ROOT / "android/host/android_main.c").read_text(encoding="utf-8")
    exported = set(re.findall(r"Java_org_stronghold_port_GameActivity_(\w+)\s*\(", glue))
    assert declared and declared <= exported, sorted(declared - exported)

    app = ET.parse(ROOT / "android/app/AndroidManifest.xml").getroot().find("application")
    activities = {item.attrib[ANDROID + "name"]: item for item in app.findall("activity")}
    aliases = app.findall("activity-alias")
    assert not aliases, "Only the main launcher may expose an app icon"
    games = [activities[name] for name in (".ClassicGameActivity", ".ExtremeGameActivity", ".StrongholdGameActivity")]
    assert all(item.attrib[ANDROID + "exported"] == "false" for item in games)
    assert activities[".LauncherActivity"].find("intent-filter") is not None
    assert activities[".LauncherActivity"].attrib[ANDROID + "screenOrientation"] == "fullSensor"
    launcher_entries = [item for item in activities.values() if any(
        c.attrib[ANDROID + "name"] == "android.intent.category.LAUNCHER"
        for intent in item.findall("intent-filter") for c in intent.findall("category"))]
    assert launcher_entries == [activities[".LauncherActivity"]]
    # Each edition loads its own library: three processes and tasks.
    assert len({item.attrib[ANDROID + "process"] for item in games}) == 3
    assert len({item.attrib[ANDROID + "taskAffinity"] for item in games}) == 3
    assert all(item.attrib[ANDROID + "launchMode"] == "singleTask" for item in games)
    assert len({item.attrib[ANDROID + "configChanges"] for item in games}) == 1
    assert ".GameActivity" not in activities, "Base JNI class must not be launchable without an edition"
    for edition in ("Classic", "Extreme", "Stronghold"):
        settings = activities[f".{edition}SettingsActivity"]
        game_activity = activities[f".{edition}GameActivity"]
        assert settings.attrib[ANDROID + "exported"] == "false"
        assert settings.attrib[ANDROID + "screenOrientation"] == "fullSensor"
        assert settings.attrib[ANDROID + "process"] == game_activity.attrib[ANDROID + "process"]
        assert settings.attrib[ANDROID + "taskAffinity"] == game_activity.attrib[ANDROID + "taskAffinity"]
        subclass = (JAVA / f"{edition}GameActivity.java").read_text()
        assert "extends GameActivity" in subclass and f'return "{edition.lower()}"' in subclass
        assert "native " not in subclass, "JNI remains declared on the common base class"
    print("Touch C/Java ABI, JNI exports, launcher/settings in all orientations and three separate edition processes: PASS")


if __name__ == "__main__":
    main()
