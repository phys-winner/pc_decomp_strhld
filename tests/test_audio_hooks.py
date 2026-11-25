from pathlib import Path
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import audio_hooks


class MilesUpsampleHooksTests(unittest.TestCase):
    def test_linear_batch_runs_once_per_entry_in_each_copy(self):
        label = b'L_2113BCA0'
        loop = (b'    goto ' + label + b';\r\n' + label + b':\r\n'
                b'    if (keep_going) { RECOMP_BACKEDGE(); goto ' + label + b'; }\r\n'
                b'    RECOMP_BACKEDGE();\r\n    goto ' + label + b';\r\n')
        source = (b'void sub_2113BCA0(void) {\r\n' + loop + b'}\r\n'
                  b'void sub_2113BCA1(void) {\r\n' + loop + b'}\r\n')
        with patch.object(audio_hooks, 'seen', set()):
            modified, copies = audio_hooks.mix_upsample(source)
            self.assertEqual(audio_hooks.seen, {'mss:2113BCA0'})
        self.assertEqual(copies, 2)
        self.assertEqual(modified.count(b'edges = stronghold_miles_upsample(state, 0u, budget);'), 2)
        self.assertEqual(modified.count(b'goto ' + label + b';'), 2)
        self.assertEqual(modified.count(b'goto ' + label + b'_scalar;'), 4)
        self.assertEqual(modified.count(b'RECOMP_BACKEDGE();'), 4)

    def test_linear_hook_rejects_a_different_loop(self):
        source = b'void sub_2113BCA0(void) {\nL_2113BCA0:\n    goto L_2113BCA0;\n}\n'
        with self.assertRaisesRegex(ValueError, 'linear mixer loop 2113BCA0 has 1 backedges'):
            audio_hooks.mix_upsample(source)
