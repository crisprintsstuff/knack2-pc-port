"""Command processor cost of benchmark runs, independent of how busy the scene is.

Usage: python scripts/cp_cost.py <benchmark log or run dir> [...]

For the frames of the last --seconds (default 30) of each run: frame time, command processor
milliseconds per frame, draws and dispatches per frame, and command processor microseconds per
draw (dispatches counted as draws), the figure to compare between builds.
"""
import argparse
import csv
from pathlib import Path
import re


def run_dir(arg):
    p = Path(arg)
    if p.is_dir():
        return p
    text = p.read_text(encoding='utf-8', errors='replace')
    found = re.findall(r'"run_dir": "([^"]+)"', text)
    return Path(found[-1].replace('\\\\', '\\'))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('runs', nargs='+')
    ap.add_argument('--seconds', type=float, default=30)
    a = ap.parse_args()
    print(f'{"run":<14}{"frames":>7}{"fps":>7}{"p50":>7}{"cp_ms":>8}{"draws":>8}{"disp":>6}'
          f'{"us/draw":>9}{"rec_wake":>9}{"upl_wait":>9}')
    for arg in a.runs:
        rows = list(csv.DictReader(open(run_dir(arg) / 'frames.csv', encoding='utf-8')))
        rows = [r for r in rows if float(r['draws'] or 0) > 100]
        tail, total = [], 0.0
        for r in reversed(rows):
            total += float(r['present_ms'] or 0)
            if total > a.seconds * 1000:
                break
            tail.append(r)
        n = max(len(tail), 1)
        f = lambda k: sum(float(r[k] or 0) for r in tail)
        present = sorted(float(r['present_ms']) for r in tail)
        cp, draws, disp = f('command_processor_ms'), f('draws'), f('dispatches')
        label = Path(arg).stem.replace('ab-', '')
        print(f'{label:<14}{len(tail):>7}{1000 * n / max(f("present_ms"), 1):>7.1f}'
              f'{present[len(present) // 2] if present else 0:>7.1f}{cp / n:>8.2f}'
              f'{draws / n:>8.0f}{disp / n:>6.0f}{1000 * cp / max(draws + disp, 1):>9.3f}'
              f'{f("recorder_wakes") / n:>9.0f}{f("upload_worker_wait_ms") / n:>9.2f}')


if __name__ == '__main__':
    main()
