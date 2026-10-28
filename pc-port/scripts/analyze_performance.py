"""Summarize measured guest flips; overlapping CPU/GPU counters are not additive."""
import argparse
import csv
import json
from pathlib import Path
import statistics

ROOT = Path(__file__).resolve().parents[1]

def summarize(path, seconds=30):
    with path.open(encoding='utf-8') as stream:
        rows = []
        for row in csv.DictReader(stream):
            try:
                rows.append({k: float(v) for k, v in row.items()})
            except (TypeError, ValueError):
                continue  # A currently running process may have a partial final row.
    window = []
    elapsed = 0
    for row in reversed(rows):
        window.append(row)
        elapsed += row['present_ms']
        if elapsed >= seconds * 1000:
            break
    if not window:
        raise ValueError('No complete frame records')
    values = sorted(r['present_ms'] for r in window)
    report = {'source': str(path), 'frames': len(window), 'window_seconds': elapsed / 1000,
              'guest_flip_fps': len(window) * 1000 / elapsed,
              'median_ms': statistics.median(values),
              'p95_ms': values[min(len(values)-1, int(len(values)*.95))],
              'max_ms': max(values), 'over_33ms': sum(v>33.333 for v in values),
              'gpu_timestamps_enabled': all(r.get('gpu_profile_enabled',0) for r in window),
              'mean_counters': {k: statistics.mean(r[k] for r in window)
                                for k in window[0] if k not in ('frame','gpu_profile_enabled')},
              'caution': 'Presented guest flips, not simulation FPS. CPU and GPU work overlap; GPU completed timestamps can arrive in later frames. Captures and profiling affect timing.'}
    path.with_name('performance-summary.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    return report

if __name__ == '__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('csv',nargs='?',type=Path)
    parser.add_argument('--seconds',type=float,default=30)
    args=parser.parse_args()
    path=args.csv or Path(json.loads((ROOT/'out/latest-run.json').read_text(encoding='utf-8'))['run_dir'])/'frames.csv'
    print(json.dumps(summarize(path,args.seconds),indent=2))
