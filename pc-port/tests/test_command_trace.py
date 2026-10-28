"""Trace validation and queue-boundary regression checks; no guest execution."""
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from analyze_command_trace import analyze, overlap


def buffer(base, size, written):
    return dict(type=0, index=0, base=hex(base), size=size, written=written)


def dispatch(seq, base=4096):
    return dict(event='dispatch', seq=seq, submit=1, flip=1, queue=1, hash='0x1',
                code='0x100', cached_code='0x100', tick=1, needs_barrier=True,
                groups=[1, 1, 1], images=[], buffers=[buffer(base, 16, True)])


class CommandTraceTests(unittest.TestCase):
    def run_trace(self, events):
        with tempfile.TemporaryDirectory() as name:
            path = Path(name) / 'trace.jsonl'
            path.write_text(''.join(json.dumps(x) + '\n' for x in events))
            return analyze(path)

    def test_incomplete_or_truncated_is_rejected(self):
        for events in ([{'event': 'header'}], [{'event': 'header'}, {'event': 'end', 'reason': 'limit_truncated'}]):
            with self.assertRaises(ValueError):
                self.run_trace(events)

    def test_queue_boundaries_do_not_leak_between_queues(self):
        report = self.run_trace([
            dict(event='header', requested_submits=1), dict(event='begin', seq=1),
            dispatch(2), dict(event='packet', seq=3, queue=0, opcode=0x3C), dispatch(4),
            dict(event='packet', seq=5, queue=1, opcode=0x58), dispatch(6),
            dict(event='submit_end', seq=7), dict(event='end', seq=7, reason='complete')])
        self.assertEqual(report['adjacent_dispatch_declared_alias_pairs'], 1)
        self.assertEqual([x['dispatches'] for x in report['segments']], [2, 1])

    def test_sequence_gap_rejected(self):
        with self.assertRaises(ValueError):
            self.run_trace([dict(event='header', requested_submits=1), dict(event='begin', seq=1),
                            dispatch(3), dict(event='submit_end', seq=4),
                            dict(event='end', seq=4, reason='complete')])

    def test_missing_submit_rejected(self):
        with self.assertRaises(ValueError):
            self.run_trace([dict(event='header', requested_submits=2), dict(event='begin', seq=1),
                            dict(event='submit_end', seq=2), dict(event='end', seq=2, reason='complete')])

    def test_adjacent_ranges_and_read_only_aliases_are_not_write_hazards(self):
        self.assertFalse(overlap(buffer(4096, 16, True), buffer(4112, 16, True)))
        self.assertFalse(overlap(buffer(4096, 16, False), buffer(4096, 16, False)))
        self.assertTrue(overlap(buffer(4096, 16, True), buffer(4100, 8, False)))


if __name__ == '__main__':
    unittest.main()
