"""Summarize aligned final CPU-phase intervals and protected-memory access sites."""
import argparse
import csv
import json
from collections import defaultdict
from pathlib import Path


def summarize(run, seconds=20):
    with (run / 'cpu-phases.csv').open() as stream:
        phases = list(csv.DictReader(stream))
    if not phases:
        raise ValueError('No completed profiling intervals')
    end = int(phases[-1]['time_ns'])
    phases = [r for r in phases if int(r['time_ns']) > end - seconds * 1e9]
    start = int(phases[0]['time_ns']) - float(phases[0]['interval_ms']) * 1e6
    flips = sum(int(r['flips']) for r in phases)
    duration = sum(float(r['interval_ms']) for r in phases)
    totals = {k: sum(float(r[k]) for r in phases) for k in phases[0]
              if k not in ('time_ns', 'interval_ms', 'flips')}
    sites = defaultdict(lambda: {'calls': 0, 'total_ms': 0., 'run_max_ms': 0.})
    with (run / 'memory-access.csv').open() as stream:
        for row in csv.DictReader(stream):
            if not start < int(row['time_ns']) <= end:
                continue
            site = sites[(int(row['kind']), row['rip'])]
            site['calls'] += int(row['calls'])
            site['total_ms'] += float(row['total_ms'])
            site['run_max_ms'] = max(site['run_max_ms'], float(row['max_ms']))
    return {'run_dir': str(run.resolve()), 'interval_ms': duration, 'flips': flips,
            'fps': flips * 1000 / duration,
            'phase_ms_per_flip': {k: v / flips for k, v in totals.items()
                                 if k.endswith('_ms')} if flips else {},
            'counter_totals': {k: v for k, v in totals.items() if not k.endswith('_ms')},
            'access_sites': [dict(kind=k[0], rip=k[1], **v)
                             for k, v in sorted(sites.items(), key=lambda x: -x[1]['total_ms'])],
            'limitations': ['Phase timings are inclusive; nested scopes must not be added.',
                            'Access times include waiting and can overlap other CPU/GPU work.',
                            'run_max_ms covers the whole run, including loading.',
                            'Site registry records the first 64 read/native and 64 write sites.',
                            'Deep instrumentation has overhead; compare with identical settings.']}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('run', type=Path)
    parser.add_argument('--seconds', type=int, default=20)
    args = parser.parse_args()
    if args.seconds <= 0:
        parser.error('--seconds must be positive')
    result = summarize(args.run, args.seconds)
    (args.run / 'deep-summary.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
    print(json.dumps(result, indent=2))
