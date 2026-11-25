"""Add launcher texts to the eleven strings.xml files.

Usage: python tools/add_strings.py FILE.json
The file holds a list of entries; each entry maps "ru" (the source text and the resource key, as I18n.key hashes
it) and "en", "uk", "de", "fr", "es", "it", "pl", "pt", "ja", "zh" to the translations. An entry with
"shared": "advice" takes its en/ru/de/pl texts from the launcher base's HyperOs.advice (android/shared), so the
key follows the shared wording. Existing keys are left as they are; the script reports them.
"""
import json
from pathlib import Path
import re
import sys
from xml.sax.saxutils import escape

ROOT = Path(__file__).resolve().parents[1]
RES = ROOT / "android/app/res"
FOLDERS = {"en": "values", "ru": "values-ru", "uk": "values-uk", "de": "values-de", "fr": "values-fr",
           "es": "values-es", "it": "values-it", "pl": "values-pl", "pt": "values-pt-rBR", "ja": "values-ja",
           "zh": "values-b+zh+Hans"}


def key(text):
    value = 0
    data = text.encode("utf-16le")
    for offset in range(0, len(data), 2):
        value = (31 * value + int.from_bytes(data[offset:offset + 2], "little")) & 0xffffffff
    return "text_" + format(value, "x")


def shared_advice():
    """The five vendors' advice of HyperOs.advice: lists of [en, ru, de, pl]."""
    source = (ROOT / "android/shared/port/launcher/HyperOs.java").read_text(encoding="utf-8")
    body = source[source.index("static String advice("):source.index("private static String pick(")]
    rows = []
    for call in re.findall(r"return pick\(column,(.*?)\);", body, re.S):
        texts = [json.loads(literal) for literal in re.findall(r'"(?:[^"\\]|\\.)*"', call)]
        if len(texts) != 4:
            raise SystemExit("HyperOs.advice: expected four languages per vendor")
        rows.append(texts)
    return rows


def main():
    entries = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
    advice = shared_advice()
    for entry in entries:
        if "advice" in entry:
            en, ru, de, pl = advice[entry.pop("advice")]
            entry.update(en=en, ru=ru, de=de, pl=pl)
        missing = set(FOLDERS) - set(entry)
        if missing:
            raise SystemExit(f"{entry.get('ru', '?')[:40]}: no {sorted(missing)}")
        specifiers = [re.findall(r"%(?:\d+\$)?[0-9.]*[dsfX]", entry[language]) for language in FOLDERS]
        if any(found != specifiers[1] for found in specifiers):
            raise SystemExit(f"{entry['ru'][:40]}: format specifiers differ between languages")
    for language, folder in FOLDERS.items():
        path = RES / folder / "strings.xml"
        raw = path.read_bytes()
        ending = "\r\n" if b"\r\n" in raw else "\n"
        text = raw.decode("utf-8").replace("\r\n", "\n")
        added = []
        for entry in entries:
            name = key(entry["ru"])
            if f'name="{name}"' in text:
                continue
            value = json.dumps(entry[language], ensure_ascii=False).replace("'", "\\'")
            added.append(f'  <string name="{name}" formatted="false">{escape(value)}</string>\n')
        if added:
            position = text.rindex("</resources>")
            path.write_text(text[:position] + "".join(added) + text[position:], encoding="utf-8", newline=ending)
        print(f"{folder}: {len(added)} added, {len(entries) - len(added)} already present")


if __name__ == "__main__":
    main()
