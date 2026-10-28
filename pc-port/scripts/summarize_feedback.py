"""Report diagnostic binding candidates and unrelated readback dependencies."""
import argparse
import csv
import json
from collections import defaultdict
from pathlib import Path


def read_csv(path):
    if not path.exists():
        return []
    with path.open() as stream:
        return list(csv.DictReader(stream))


def summarize(run):
    bindings = read_csv(run / 'feedback-bindings.csv')
    dependencies = read_csv(run / 'readback-dependencies.csv')
    if not bindings and not dependencies:
        raise ValueError('No feedback trace found; enable ISS_DEEP_PROFILE and ISS_FEEDBACK_TRACE')
    end = max(int(r['time_ns']) for r in bindings + dependencies)
    start = end - 20_000_000_000
    bindings = [r for r in bindings if int(r['time_ns']) > start]
    dependencies = [r for r in dependencies if int(r['time_ns']) > start]
    candidates = defaultdict(lambda: {'reads': 0, 'fault_ms': 0.})
    for row in bindings:
        candidate = candidates[(row['rip'], row['shader_hash'])]
        candidate['reads'] += 1
        candidate['fault_ms'] += float(row['elapsed_ms'])
    different = [r for r in dependencies
                 if r['window_gfx_tick'] != r['request_gfx_tick']
                 or r['window_compute_tick'] != r['request_compute_tick']]
    return {'run': str(run.resolve()), 'tail_seconds': 20,
            'matched_reads': len(bindings), 'readback_requests': len(dependencies),
            'different_window_dependencies': len(different),
            'request_done_but_window_pending': sum(r['request_done'] == '1' and
                                                 r['window_done'] == '0' for r in different),
            'candidates': sorted([dict(rip=k[0], shader=k[1], **v) for k, v in candidates.items()],
                                 key=lambda x: -x['fault_ms']),
            'limitations': ['Most recent matching binding is a candidate, not a completed GPU write.',
                            'Fault time includes other queued work; it is not shader execution time.',
                            'History is bounded to the last 8192 small writable bindings.',
                            'Request dependencies use memory tracker granularity, not byte-level provenance.',
                            'Trace adds locking and file I/O overhead; keep it disabled in normal play.']}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('run', type=Path)
    args = parser.parse_args()
    result = summarize(args.run)
    (args.run / 'feedback-summary.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
    print(json.dumps(result, indent=2))
