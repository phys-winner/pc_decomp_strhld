"""Check complete GOG locale coverage and formatting in the port's Android resources."""
import json
from pathlib import Path
import re
import unittest
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
RES = ROOT / 'android/app/res'


def key(text):
    value = 0
    data = text.encode('utf-16le')
    for offset in range(0, len(data), 2):
        value = (31 * value + int.from_bytes(data[offset:offset+2], 'little')) & 0xffffffff
    return 'text_' + format(value, 'x')


def resources(folder):
    return {item.attrib['name']: item.text or '' for item in ET.parse(RES / folder / 'strings.xml').getroot()}


class PortLocalizationTests(unittest.TestCase):
    def test_all_gog_languages_have_complete_translations(self):
        folders = ['values', 'values-ru', 'values-uk', 'values-de', 'values-fr', 'values-es',
                   'values-it', 'values-pl', 'values-pt-rBR', 'values-ja', 'values-b+zh+Hans']
        base = resources('values')
        original = resources('values-ru')
        self.assertGreater(len(base), 450)
        for folder in folders:
            translated = resources(folder)
            self.assertEqual(set(base), set(translated), folder)
            for name, value in translated.items():
                self.assertTrue(value.strip('"').strip(), (folder, name))
                self.assertNotIn('[[[S', value)
                self.assertNotIn('[BR]', value)
                self.assertEqual(re.findall(r'%(?:\d+\$)?[0-9.]*[dsfX]', original[name]),
                                 re.findall(r'%(?:\d+\$)?[0-9.]*[dsfX]', value), (folder, name))

    def test_source_strings_and_original_ucp_descriptions_are_covered(self):
        base, russian = resources('values'), resources('values-ru')
        names = ['LauncherActivity', 'SettingsActivity', 'GameActivity', 'GameFiles',
                 'GameMods', 'GameImages', 'GogInstaller', 'UcpOptions', 'I18n', 'ControlLayout']
        texts = set()
        for name in names:
            source = (ROOT / 'android/app/src/org/stronghold/port' / (name + '.java')).read_text(encoding='utf-8')
            for literal in re.findall(r'"(?:[^"\\]|\\.)*"', source):
                if re.search('[А-Яа-яЁё]', literal):
                    texts.add(json.loads(literal))
        catalog = json.loads((ROOT / 'generated/ucp/catalog.json').read_text(encoding='utf-8'))
        for feature in catalog['features']:
            texts.update(feature[field] for field in ('name', 'description') if feature[field].strip())
        texts.update(category['name'] for category in catalog['categories'])
        keys = [key(text) for text in texts]
        self.assertEqual(len(keys), len(set(keys)), 'Source resource hash collision')
        self.assertLessEqual(set(keys), set(base))
        self.assertEqual(set(base), set(russian))

    def test_main_cjk_controls_do_not_fall_back_to_english(self):
        labels = ['Выбрать папку…', 'Распаковать ZIP…', 'Установить из GOG…',
                  'Язык интерфейса…', 'Настройки', 'Авто · ',
                  'Разрешение и пропорции применяются при новом запуске игры']
        for folder in ['values-ja', 'values-b+zh+Hans']:
            translated = resources(folder)
            for text in labels:
                self.assertRegex(translated[key(text)], '[\u3040-\u30ff\u3400-\u9fff]', (folder, text))

    def test_chinese_port_text_has_no_untranslated_english_sentences(self):
        chinese = resources('values-b+zh+Hans')
        for name, source in resources('values-ru').items():
            if re.search('[А-Яа-яЁё]', source):
                self.assertNotRegex(chinese[name], r'[A-Za-z]{2,}(?:[ ,]+[A-Za-z]{2,}){2,}', name)

    def test_russian_patch_titles_are_labels_and_descriptions_are_readable(self):
        russian = resources('values-ru')
        catalog = json.loads((ROOT / 'generated/ucp/catalog.json').read_text(encoding='utf-8'))
        for feature in catalog['features']:
            name = json.loads(russian[key(feature['name'])].replace("\\'", "'"))
            self.assertNotRegex(name, r'[.!?…]$')
            self.assertNotEqual(name, feature['id'])
            self.assertNotIn('AI', name)
            if feature['description'].strip():
                description = json.loads(russian[key(feature['description'])].replace("\\'", "'"))
                self.assertNotIn('&', description, feature['id'])


if __name__ == '__main__':
    unittest.main()
