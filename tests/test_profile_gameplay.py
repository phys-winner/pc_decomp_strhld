"""Pure parsing/math checks; no Android device is contacted."""
import importlib.util
from pathlib import Path
import unittest
from unittest.mock import patch
from types import SimpleNamespace
import subprocess

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("profile_gameplay", ROOT / "tools/profile_gameplay.py")
PROFILE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PROFILE)


def stat(ticks, tid=123, name="guest ) thread", start=900):
    fields = ["S"] + ["0"] * 49
    fields[11], fields[12], fields[19] = str(ticks), "0", str(start)
    return f"{tid} ({name}) " + " ".join(fields)


class GameplayProfileTest(unittest.TestCase):
    def test_stat_and_snapshot(self):
        self.assertEqual(PROFILE.parse_stat(stat(81))["ticks"], 81)
        self.assertEqual(PROFILE.parse_stat(stat(81))["name"], "guest ) thread")
        snapshot = PROFILE.parse_snapshot("\n".join([stat(81), stat(80), "cpu 10 20 30 40 50 60 70 80 1 2", "100.5 8"]))
        self.assertEqual(snapshot["device_ticks"], 360)
        self.assertEqual(snapshot["device_idle"], 90)
        with self.assertRaises(ValueError):
            PROFILE.parse_snapshot("bad")

    def test_cpu_uses_elapsed_time_not_whole_device_denominator(self):
        def snapshot(at, ticks, device_ticks, idle):
            return {"uptime": at, "process": PROFILE.parse_stat(stat(ticks)),
                    "threads": [PROFILE.parse_stat(stat(ticks))],
                    "device_ticks": device_ticks, "device_idle": idle}
        samples = [snapshot(100, 100, 10000, 8000), snapshot(102, 250, 11600, 9200)]
        result = PROFILE.cpu_summary(samples, 100, 8)
        self.assertEqual(result["cpu_percent"], 75)
        self.assertEqual(result["cpu_device_capacity_percent"], 9.375)
        self.assertEqual(result["device_cpu_percent"], 25)
        self.assertEqual(result["threads"][0]["process_cpu_share_percent"], 100)
        samples[-1]["process"]["start_ticks"] += 1
        with self.assertRaises(ValueError):
            PROFILE.cpu_summary(samples, 100, 8)


    def test_frame_ring_deduplicates_repeated_samples(self):
        frames = [(1000000000, 10), (1020000000, 12), (1040000000, 11)]
        result = PROFILE.frame_summary([{"frames": frames[:2], "camera": [100, 200]},
                                        {"frames": frames[1:], "camera": [120, 210]}],.06)
        self.assertEqual(result["frames"], 3)
        self.assertEqual(result["fps"], 50)
        self.assertEqual(result["frame_p95_ms"], 20)
        self.assertEqual(result["busy_wall_mean_ms"], 11)
        self.assertEqual(result["cpu_ms_per_frame"],20)
        self.assertTrue(result["camera_moved"])

    def test_memory_retries_only_diagnostic_reads(self):
        failure=subprocess.CalledProcessError(4294967295,["adb"],stderr=b"transient transport failure")
        results=[failure,SimpleNamespace(stdout=b"x",stderr=b""),
                 SimpleNamespace(stdout=b"abcd",stderr=b"")]
        with patch.object(PROFILE,"run",side_effect=results) as run,patch.object(PROFILE.time,"sleep") as sleep:
            self.assertEqual(PROFILE.read_native_memory(["adb","-s","test"],123,0x1001,2),b"bc")
            self.assertEqual(run.call_count,3)
            self.assertEqual(sleep.call_count,2)
            self.assertTrue(all("dd if=/proc/123/mem" in call.args[0][-1] for call in run.call_args_list))

    def test_frame_counter_above_uint32_uses_complete_count_and_timing_bounds(self):
        base=1<<40
        probes=[{"frame_count":base,"host_monotonic":100,"host_monotonic_end":100.1},
                {"frame_count":base+60,"host_monotonic":102,"host_monotonic_end":102.1}]
        result=PROFILE.frame_summary(probes,1.2)
        self.assertEqual(result["frame_counter_delta"],60)
        self.assertEqual(result["fps_counter"],30)
        self.assertEqual(result["cpu_ms_per_frame"],20)
        self.assertEqual(result["cpu_ms_per_frame_source"],"frame_counter")
        self.assertLess(result["fps_counter_lower_bound"],30)
        self.assertGreater(result["fps_counter_upper_bound"],30)

    def test_frame_counter_reset_mid_capture_rejects_delta_even_if_final_count_grew(self):
        probes=[{"frame_count":100,"host_monotonic":1},
                {"frame_count":0,"host_monotonic":2},
                {"frame_count":150,"host_monotonic":3}]
        result=PROFILE.frame_summary(probes,1)
        self.assertTrue(result["counter_reset"])
        self.assertIsNone(result["frame_counter_delta"])
        self.assertIsNone(result["fps_counter"])
        self.assertIsNone(result["cpu_ms_per_frame"])

    def test_frame_cutoff_excludes_old_entry_missed_by_first_snapshot(self):
        probes=[{"frames": [(2000,10),(3000,11)],"camera": [1,2]},
                {"frames": [(1000,7),(2000,10),(3000,11),(4000,12)],"camera": [3,4]}]
        measured=PROFILE.recorded_probes(probes)
        self.assertEqual(measured[0]["frames"],[])
        self.assertEqual(measured[1]["frames"],[(4000,12)])
        self.assertEqual(measured[1]["camera"],[3,4])
        self.assertEqual(probes[1]["frames"][0],(1000,7))

    def test_memory_short_read_stops_after_three_attempts_with_stderr(self):
        with patch.object(PROFILE,"run",return_value=SimpleNamespace(stdout=b"x",stderr=b"diagnostic")) as run, \
                patch.object(PROFILE.time,"sleep") as sleep:
            with self.assertRaisesRegex(RuntimeError,"after 3 attempts: short read.*diagnostic"):
                PROFILE.read_native_memory(["adb"],123,0x1000,4)
            self.assertEqual(run.call_count,3)
            self.assertEqual(sleep.call_count,2)


if __name__ == "__main__":
    unittest.main()
