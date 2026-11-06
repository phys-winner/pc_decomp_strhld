import gzip
import json
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import prepare_compat


class CompatibilityTests(unittest.TestCase):
    def test_round_trip_extends_and_truncates(self):
        for source, target in [(b'abcdef', b'abX'), (b'abX', b'abcdef'), (b'', b'abc')]:
            patch = prepare_compat.make_patch(source, target)
            self.assertEqual(prepare_compat.apply_patch(source, patch), target)
            self.assertEqual(patch, prepare_compat.make_patch(source, target))
            with self.assertRaisesRegex(ValueError, 'input mismatch'):
                prepare_compat.apply_patch(source + b'X', patch)
            with self.assertRaises((ValueError, EOFError, gzip.BadGzipFile)):
                prepare_compat.apply_patch(source, patch[:-3])

    def test_catalog_matches_java(self):
        catalog = json.loads(prepare_compat.CATALOG.read_text(encoding='utf-8'))
        self.assertEqual(prepare_compat.JAVA.read_text(encoding='utf-8'), prepare_compat.java_source(catalog))
        for entries in catalog['languages'].values():
            for digest, language in entries.items():
                self.assertRegex(digest, r'^[0-9a-f]{64}$')
                self.assertTrue(language['tag'] and language['name'])


if __name__ == '__main__':
    unittest.main()
