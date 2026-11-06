import json
import copy
import re
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import lift_core
import ucp
import capstone
from tools.disasm.disasm32 import Instruction


class UcpTests(unittest.TestCase):
    def setUp(self):
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        self.md.detail = True

    def instructions(self, data, address=0x1000):
        return [Instruction(i.address, i.size, i.mnemonic, i.op_str, bytes(i.bytes), list(i.operands))
                for i in self.md.disasm(data, address)]

    def test_common_instruction_boundary(self):
        before = bytes.fromhex('40 40 40 40 40 c3')
        after = bytes.fromhex('b8 01 00 00 00 c3')
        areas = ucp.patch_areas(self.instructions(before), before, after, 0x1000, self.md)
        self.assertEqual([(a,b) for a,b,_ in areas], [(0x1000,0x1005)])
        self.assertEqual([i.mnemonic for i in areas[0][2]], ['mov'])

    def test_skipped_data_is_not_code(self):
        code = bytes.fromhex('eb 02 cd 51 c3')
        instructions = ucp.walk_code(self.md, [(0x1000,code)], [0x1000])
        self.assertEqual([i.address for i in instructions], [0x1000,0x1004])
        with self.assertRaisesRegex(ValueError, '[Oo]verlap'):
            ucp.walk_code(self.md, [(0x1000,bytes.fromhex('b8 01 00 00 00 c3'))], [0x1000,0x1001])

    def test_word_flags_use_word_stack(self):
        lifter = lift_core.CheckedLifter(precise_carry=True, precise_sbb=True)
        push, pop = self.instructions(bytes.fromhex('66 9c 66 9d'))
        self.assertIn('esp -= 2; MEM16(esp)', '\n'.join(lifter.lift_instruction(push)))
        restored = '\n'.join(lifter.lift_instruction(pop))
        self.assertIn('esp += 2', restored)
        self.assertIn('0xffff0000u', restored)
        self.assertIsNone(lifter._flag_state)

    def test_generated_profile_is_pinned_and_guarded(self):
        catalog = json.loads((ucp.OUT/'catalog.json').read_text(encoding='utf-8'))
        self.assertEqual(catalog['upstream'], ucp.PIN)
        for module, edition in [('classic','classic'),('game','extreme')]:
            functions = json.loads((ucp.OUT/f'{module}-functions.json').read_text())
            records = json.loads((ucp.OUT/f'{module}-runtime.json').read_text())
            self.assertGreater(len(records), 40)
            self.assertTrue(all('if (stronghold_ucp_enabled[' in body for body in functions.values()))
            for source in (ucp.ROOT/f'generated/{module}-x86').glob('recomp_*.c'):
                for match in re.finditer(r'(?m)^void sub_([0-9A-F]{8})\(void\) \{.*?^\}', source.read_text(), re.DOTALL):
                    if match[1] not in functions:
                        continue
                    original, replacement = match[0], functions[match[1]]
                    for expression in re.findall(r'_itail_tgt = \(uint32_t\)\((.*?)\); goto _ljump;', original):
                        self.assertTrue(expression in replacement, 'UCP adapter discarded an original indirect jump')
                    original_cases = set(re.findall(r'case 0x([0-9A-F]{8})u: goto L_', original))
                    replacement_cases = set(re.findall(r'case 0x([0-9A-F]{8})u: goto L_', replacement))
                    self.assertLessEqual(original_cases, replacement_cases, 'Original switch targets disappeared')
            for index, record in records.items():
                self.assertTrue(catalog['features'][int(index)]['editions'][edition]['available'])
                for block in record['allocations']:
                    self.assertGreaterEqual(block['address'], 0x50000000+int(index)*0x100000)
                    self.assertLess(block['address']+len(bytes.fromhex(block['data'])), 0x50100000+int(index)*0x100000)

    def test_categories_and_credits_do_not_change_patch_data(self):
        catalog = json.loads((ucp.OUT/'catalog.json').read_text(encoding='utf-8'))
        before = copy.deepcopy(catalog['features'])
        ucp.catalog_metadata(catalog)
        self.assertEqual(catalog['features'], before)
        groups = {category['id'] for category in catalog['categories']}
        self.assertEqual(len(groups), 6)
        for feature in catalog['features']:
            self.assertIn(feature['category'], groups)
            self.assertEqual(len({a['name'] for a in feature['authors']}), len(feature['authors']))
            for author in feature['authors']:
                self.assertRegex(author['source'], r'^https://github.com/UnofficialCrusaderPatch/[^/]+/commit/[0-9a-f]{40}$')
        by_id = {feature['id']:feature for feature in catalog['features']}
        self.assertEqual(by_id['ai_access']['category'], 'ai_fixes')
        self.assertEqual(by_id['ai_housing']['category'], 'ai_improvements')
        self.assertEqual(by_id['o_healer']['category'], 'balance_changes')
        self.assertEqual([a['name'] for a in by_id['o_stop_player_keep_rotation']['authors']], ['gynt'])
        self.assertEqual({a['name'] for a in by_id['ai_housing']['authors']}, {'patel-nikhil','Krarilotus'})


if __name__ == '__main__':
    unittest.main()
