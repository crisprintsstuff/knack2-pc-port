"""Small synthetic gates for the streaming runtime timeline analyzer."""

import csv
import importlib.util
import tempfile
import unittest
from pathlib import Path


SCRIPT = Path(__file__).resolve().parents[1] / "scripts" / "analyze_runtime_timeline.py"
SPEC = importlib.util.spec_from_file_location("analyze_runtime_timeline", SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)
FIELDS = ("event", "cpu_ns", "observed_guest_frame", "thread_id", "owner", "tick",
          "a", "b", "c", "d", "duration_ns")


def event(name, ns, owner=0, tick=0, a=0, b=0, c=0, d=0, duration=0):
    return dict(zip(FIELDS, (name, ns, 10, 123, owner, tick, a, b, c, d, duration)))


class TimelineTest(unittest.TestCase):
    def test_joins_health_missing_wrap_and_reused_frame(self):
        rows = [
            event("trace_health", 1, a=20, b=1200, c=5),
            event("guest_queue_submit", 10, owner=7, tick=9, a=1, b=3),
            event("cp_asc_begin", 30, owner=7, tick=9, a=1, b=3),
            event("cp_complete", 60, owner=7, tick=9, a=1, b=3),
            event("guest_queue_submit", 70, owner=7, tick=10, a=0, b=3),
            event("session_enqueued", 100, owner=77, tick=5, a=2),
            event("recorder_submit_begin", 120, owner=77, tick=5),
            event("vk_queue_submit_begin", 150, owner=77, tick=5),
            event("vk_queue_submit_end", 160, owner=77, tick=5, a=5),
            event("gpu_session_valid", 250, owner=77, tick=5, a=100, b=200, c=1000),
            event("gpu_session_valid", 260, owner=77, tick=6, a=150, b=250, c=1000),
            event("gpu_session_valid", 270, owner=77, tick=7, a=10, b=5, c=1000),
            event("completion_observed", 280, owner=77, tick=7, a=4),
            event("vo_flip_accept", 300, owner=1),
            event("vo_prepare_end", 320, owner=1, a=999, b=101),
            event("vo_queue_push", 330, owner=1),
            event("vo_queue_pop", 350, owner=1),
            event("present_begin", 355, owner=999, tick=101),
            event("present_end", 370, owner=999, tick=101),
            event("vo_present_return", 380, owner=1),
            event("vo_flip_accept", 400, owner=2),
            event("vo_prepare_end", 420, owner=2, a=999, b=102),
            event("present_begin", 440, owner=999, tick=102),
            event("present_end", 470, owner=999, tick=102),
            event("user_stutter_mark", 480, a=12345),
            event("trace_truncated", 500, c=8),
        ]
        with tempfile.TemporaryDirectory() as directory:
            run_dir = Path(directory)
            with (run_dir / "runtime-timeline.csv").open("w", newline="") as stream:
                writer = csv.DictWriter(stream, FIELDS)
                writer.writeheader()
                writer.writerows(rows)
            summary = MODULE.analyze(run_dir)
            self.assertEqual(summary["trace_health"]["max_dropped"], 8)
            self.assertTrue(summary["trace_health"]["truncated"])
            self.assertEqual(summary["joins_completed"]["guest_queue_to_cp_asc"], 1)
            self.assertEqual(summary["incomplete_in_retained_maps"]["guest_submits_without_cp_begin"], 1)
            self.assertEqual(summary["joins_completed"]["session_enqueued_to_vk_submit_begin"], 1)
            self.assertEqual(summary["joins_completed"]["present_frame_join"], 2)
            self.assertEqual(summary["invalid_or_skipped"]["gpu_timestamp_wrap_or_period"], 1)
            self.assertEqual(summary["gpu_raw_by_scheduler"]["77"]["union_ns"], 150)
            self.assertEqual(summary["completion_observed_watermarks"]["77"]["known_tick"], 7)
            self.assertEqual(summary["user_stutter_marks"][0]["sdl_tick_ms"], 12345)
            self.assertTrue((run_dir / "timeline-summary.json").is_file())

    def test_pending_cap_eviction(self):
        analyzer = MODULE.Analyzer()
        original = MODULE.LIMITS["guest_submits"]
        MODULE.LIMITS["guest_submits"] = 2
        try:
            for i in range(3):
                analyzer.event({key: str(value) for key, value in event(
                    "guest_queue_submit", i + 1, owner=1, tick=i + 1, a=0, b=1).items()})
            self.assertEqual(analyzer.evictions["guest_submits"], 1)
            self.assertEqual(len(analyzer.pending["guest_submits"]), 2)
        finally:
            MODULE.LIMITS["guest_submits"] = original


if __name__ == "__main__":
    unittest.main()
