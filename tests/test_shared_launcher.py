"""The classes taken from the shared launcher base stay unchanged copies, and the port starts on Android 8."""
import json
from pathlib import Path
import re
import sys
import unittest
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
SHARED = ROOT / "android/shared/port/launcher"
BASE = ROOT.parent / "launcher/android/src/port/launcher"
LOCAL = ROOT / "android/app/src/port/launcher"


class SharedLauncherTests(unittest.TestCase):
    def test_copies_match_the_base_beside_the_project(self):
        if not BASE.is_dir():
            self.skipTest("the launcher base is not checked out beside the project")
        names = sorted(path.name for path in SHARED.glob("*.java"))
        self.assertEqual(names, ["ArchiveMd5.java", "FpsCounter.java", "FpsOverlay.java", "HudField.java",
                                 "HyperOs.java", "State.java", "SystemInfo.java"])
        for name in names:
            self.assertEqual((SHARED / name).read_bytes().replace(b"\r\n", b"\n"),
                             (BASE / name).read_bytes().replace(b"\r\n", b"\n"),
                             f"{name}: refresh android/shared from the launcher base")

    def test_renderer_failure_header_matches_the_base(self):
        base = ROOT.parent / "launcher/native/replacements/renderer_failure.h"
        if not base.is_file():
            self.skipTest("the launcher base is not checked out beside the project")
        copy = ROOT / "android/shared/native/replacements/renderer_failure.h"
        self.assertEqual(copy.read_bytes().replace(b"\r\n", b"\n"), base.read_bytes().replace(b"\r\n", b"\n"),
                         "refresh android/shared/native from the launcher base")

    def test_renderer_failure_texts_follow_the_base(self):
        """The renderer failure dialog uses the base's RF_* rows (en/ru/de/pl as the base has them) in all 11 languages."""
        texts = ROOT.parent / "launcher/android/src/port/launcher/Texts.java"
        if not texts.is_file():
            self.skipTest("the launcher base is not checked out beside the project")
        import add_strings
        source = texts.read_text(encoding="utf-8")
        rows = {}
        for row in re.findall(r'\{("(?:[^"\\]|\\.)*"(?:\s*,\s*"(?:[^"\\]|\\.)*"){3})\}', source):
            cells = [json.loads(cell) for cell in re.findall(r'"(?:[^"\\]|\\.)*"', row)]
            rows.setdefault(cells[0], cells)
        english = ["The renderer failed",
                   "%s could not start on this device, so the game was stopped. No other renderer was started in its place.",
                   "With %s the graphics driver stopped responding (the GPU device was lost), so the game was stopped. "
                   "No other renderer was started in its place.",
                   "Details: %s", "No other renderer is available on this device.",
                   "The last start stopped because %1$s failed: %2$s"]
        values = {folder: {item.attrib["name"]: item.text for item in
                           ET.parse(ROOT / "android/app/res" / folder / "strings.xml").getroot()}
                  for folder in add_strings.FOLDERS.values()}
        for text in english:
            self.assertIn(text, rows, text)
            en, ru, de, pl = rows[text]
            name = add_strings.key(ru)
            for language, folder in add_strings.FOLDERS.items():
                self.assertIn(name, values[folder], (folder, text))
            for language, expected in (("en", en), ("ru", ru), ("de", de), ("pl", pl)):
                stored = values[add_strings.FOLDERS[language]][name]
                self.assertEqual(json.loads(stored.replace("\\'", "'")), expected, (language, text))
        launcher = (ROOT / "android/app/src/org/stronghold/port/LauncherActivity.java").read_text(encoding="utf-8")
        for text in english:
            self.assertIn(json.dumps(rows[text][1], ensure_ascii=False), launcher)

    def test_local_classes_do_not_shadow_copied_ones(self):
        self.assertFalse({path.name for path in SHARED.glob("*.java")} & {path.name for path in LOCAL.glob("*.java")})

    def test_counter_is_off_on_a_fresh_install(self):
        spec = (LOCAL / "Spec.java").read_text(encoding="utf-8")
        self.assertIn('new Option(FPS_COUNTER, "0")', spec)
        shared = (LOCAL / "Shared.java").read_text(encoding="utf-8")
        self.assertIn('State.get(context, State.CHOICES, FPS, "0")', shared)

    def test_game_mode_advice_has_all_port_languages(self):
        import add_strings
        for texts in add_strings.shared_advice():
            name = add_strings.key(texts[1])
            for language, folder in add_strings.FOLDERS.items():
                values = {item.attrib["name"]: item.text for item in
                          ET.parse(ROOT / "android/app/res" / folder / "strings.xml").getroot()}
                self.assertIn(name, values, (folder, texts[1][:40]))
                if language in ("en", "ru", "de", "pl"):
                    column = ("en", "ru", "de", "pl").index(language)
                    self.assertEqual(json.loads(values[name].replace("\\'", "'")), texts[column])

    def test_minimum_android_is_the_same_everywhere(self):
        import build_android
        manifest = (ROOT / "android/app/AndroidManifest.xml").read_text(encoding="utf-8")
        minimum = int(re.search(r'android:minSdkVersion="(\d+)"', manifest)[1])
        self.assertEqual(minimum, 26)
        self.assertEqual(build_android.API, minimum)
        self.assertIn(f"-DANDROID_PLATFORM=android-{minimum}", (ROOT / "tools/build_innoextract.py").read_text(encoding="utf-8"))
        self.assertIn('"--min-api", "26"', (ROOT / "tools/build_apk.py").read_text(encoding="utf-8"))
        # 2D game: OpenGL ES 3.0 for the layered presentation, no Vulkan feature or permission.
        self.assertIn('android:glEsVersion="0x00030000"', manifest)
        self.assertNotIn("vulkan", manifest.lower())

    def test_api28_audio_symbol_is_resolved_at_run_time(self):
        source = (ROOT / "android/host/android_main.c").read_text(encoding="utf-8")
        self.assertNotRegex(source, r"(?<!\")AAudioStreamBuilder_setUsage\(")
        self.assertIn('dlsym(library, "AAudioStreamBuilder_setUsage")', source)


if __name__ == "__main__":
    unittest.main()
