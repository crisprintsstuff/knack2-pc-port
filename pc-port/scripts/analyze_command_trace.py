"""Validate and summarize bounded command traces without claiming shader independence.

Submit intervals are CP OnSubmit intervals, not automatically simulation frames.
An overlap is conservative: descriptor extent is not the shader's accessed subset.
"""
import argparse
from collections import Counter, defaultdict
import csv
import json
from pathlib import Path


def overlap(a, b):
    if a.get('type') != 0 or b.get('type') != 0 or not (a['written'] or b['written']):
        return False
    start_a, start_b = int(a['base'], 16), int(b['base'], 16)
    return bool(a['size'] and b['size'] and max(start_a, start_b) < min(start_a + a['size'], start_b + b['size']))


def analyze(path):
    with path.open(encoding='utf-8') as stream:
        rows = [json.loads(line) for line in stream]
    if not rows or rows[0].get('event') != 'header' or rows[-1].get('event') != 'end' or rows[-1].get('reason') != 'complete':
        raise ValueError('Incomplete/truncated trace: reject coverage claims')
    seqs = [r['seq'] for r in rows if 'seq' in r and r['event'] != 'end']
    if seqs != list(range(1, len(seqs) + 1)):
        raise ValueError('Trace sequence gap/reordering')
    if (sum(r['event'] == 'begin' for r in rows) != 1 or
        sum(r['event'] == 'submit_end' for r in rows) != rows[0]['requested_submits']):
        raise ValueError('Captured submit count does not match requested coverage')
    segments, previous, segments_data = defaultdict(int), {}, defaultdict(list)
    dispatches, alias_pairs, sync, indirect = [], [], [], []
    packet_counts = Counter()
    for row in rows:
        event = row['event']
        if event == 'packet':
            q, op = row['queue'], row['opcode']
            packet_counts[(q, op)] += 1
            if q > 0 and op != 0x15:
                segments[q] += 1
                previous.pop(q, None)
            if op in (0x49, 0x3C, 0x58, 0x47, 0x48, 0x50, 0x40):
                sync.append(row)
        elif event in ('dispatch', 'dispatch_indirect'):
            q = row['queue']
            guest = [b for b in row['buffers'] if b['type'] == 0]
            feedback = [b['base'] for b in guest if b['written'] and b['size'] == 48]
            entry = {k: row[k] for k in ('seq', 'submit', 'flip', 'queue', 'hash', 'code', 'tick', 'needs_barrier')}
            entry.update(segment=segments[q], groups='x'.join(map(str, row['groups'])),
                         feedback=';'.join(feedback), records_0x60=';'.join(b['base'] for b in guest if b['size'] == 0xC00000),
                         group_pairs=';'.join(b['base'] for b in guest if b['size'] == 0x100000),
                         gds=any(b['type'] == 4 for b in row['buffers']), image_count=len(row['images']))
            dispatches.append(entry)
            segments_data[(row['submit'], q, segments[q])].append(row)
            prev = previous.get(q)
            if prev and q > 0:
                pairs = [(a, b) for a in prev['buffers'] for b in row['buffers'] if overlap(a, b)]
                if pairs:
                    alias_pairs.append({'before_seq': prev['seq'], 'after_seq': row['seq'], 'queue': q,
                                        'before_hash': prev['hash'], 'after_hash': row['hash'],
                                        'overlaps': [{'before_index': a['index'], 'after_index': b['index'],
                                                      'start': hex(max(int(a['base'], 16), int(b['base'], 16))),
                                                      'before_size': a['size'], 'after_size': b['size'],
                                                      'before_written': a['written'], 'after_written': b['written']}
                                                     for a, b in pairs]})
            previous[q] = row
        elif event == 'draw_indirect':
            indirect.append(row)
    counts = Counter((r['submit'], r['queue'], r['hash']) for r in dispatches)
    report = {'source': str(path.resolve()), 'complete': True,
              'event_counts': dict(Counter(r['event'] for r in rows)),
              'distinct_compute_hashes': len({r['hash'] for r in dispatches}),
              'dispatch_counts': [{'submit': s, 'queue': q, 'hash': h, 'count': n} for (s, q, h), n in sorted(counts.items())],
              'packet_counts': [{'queue': q, 'opcode': op, 'count': n} for (q, op), n in sorted(packet_counts.items())],
              'compute_code_cache_mismatches': sum(r['code'] != r['cached_code'] for r in rows if r['event'].startswith('dispatch')),
              'segments': [{'submit': s, 'queue': q, 'segment': seg, 'dispatches': len(v), 'first_seq': v[0]['seq'], 'last_seq': v[-1]['seq'],
                            'hash_counts': dict(Counter(r['hash'] for r in v))} for (s, q, seg), v in sorted(segments_data.items())],
              'adjacent_dispatch_declared_alias_pairs': len(alias_pairs),
              'limitations': ['CPU translation order is not GPU completion order.',
                             'Submit and flip indices do not prove a simulation-frame boundary.',
                             'Buffer write flag does not distinguish atomics, reads or write subranges.',
                             'Special GDS buffers and images are recorded, not resolved to byte-level hazards.',
                             'Register/NOP/config/context packets are omitted; bound descriptors are captured instead.',
                             'Graphics code field uses cached stage address; compute code is from current CS registers.',
                             'HLE/empty/skipped dispatches may have a packet and no dispatch stage event.']}
    dest = path.parent
    with (dest / 'dispatch-map.csv').open('w', newline='', encoding='utf-8') as f:
        writer = csv.DictWriter(f, fieldnames=list(dispatches[0]) if dispatches else ['seq'])
        writer.writeheader(); writer.writerows(dispatches)
    for name, value in [('command-summary.json', report), ('dispatch-alias-candidates.json', alias_pairs),
                        ('sync-packets.json', sync), ('indirect-draw-bindings.json', indirect)]:
        (dest / name).write_text(json.dumps(value, indent=2) + '\n', encoding='utf-8')
    return report


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('trace', type=Path)
    args = ap.parse_args()
    report = analyze(args.trace)
    print(json.dumps({k: report[k] for k in ('complete', 'event_counts', 'distinct_compute_hashes', 'adjacent_dispatch_declared_alias_pairs', 'compute_code_cache_mismatches')}, indent=2))
