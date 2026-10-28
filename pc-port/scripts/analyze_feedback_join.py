"""Summarize candidate associations; this does not prove the final GPU writer."""
import argparse
from collections import Counter
import csv
import json
from pathlib import Path
import statistics


def analyze(path):
    with path.open(newline='', encoding='utf-8') as source:
        rows = list(csv.DictReader(source))
    starts = {r['ticket']: r for r in rows if r['event'] == 'read_begin'}
    matched = []
    hashes = Counter()
    states = Counter()
    missing = changed = 0
    for end in rows:
        if end['event'] != 'read_end' or end['ticket'] not in starts:
            continue
        start = starts[end['ticket']]
        elapsed = (int(end['ns']) - int(start['ns'])) / 1e6
        matched.append(elapsed)
        if int(start['writer_sequence']) == 0:
            missing += 1
            continue
        hashes[start['shader_hash']] += 1
        tick = int(start['writer_tick'])
        states['complete_known' if tick <= int(start['known_complete_tick']) else
               'submitted' if tick <= int(start['submitted_tick']) else
               'not_submitted'] += 1
        changed += int(end.get('latest_sequence', end['writer_sequence'])) != int(start['writer_sequence'])
    return dict(
        source=str(path.resolve()), rows=len(rows), reads=len(matched),
        missing_candidate=missing, later_candidate_changed=changed,
        candidate_hashes=dict(hashes), candidate_states=dict(states),
        handler_ms_total=sum(matched),
        handler_ms_mean=statistics.mean(matched) if matched else None,
        handler_ms_p95=sorted(matched)[int((len(matched)-1)*.95)] if matched else None,
        limitation='Candidates are bound descriptors, not proof of executed writes; '
                   'mapping epoch is not arena generation; known completion is a lower bound.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('path', type=Path)
    args = parser.parse_args()
    print(json.dumps(analyze(args.path), indent=2))
