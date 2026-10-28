"""A/B of arbitrary environment variants on identical isolated save snapshots.

    python benchmark_env.py --rounds 2 --seconds 80 "A:" "B:ISS_UPLOAD_WORKER=1" "C:ISS_TUNING=scout=1"

Each variant is "label:VAR=value;VAR=value" applied over the launcher baseline (render reuse,
parallel render, descriptor fast path, GPU profile 1). Variants run interleaved (A B C A B C)
so that warm caches favour none. Runs that fail, stop early or report events are rejected.
Reports the last 30 seconds of each run and per-variant means in comparison.json.
"""
import argparse
import csv
from datetime import datetime
import json
import os
from pathlib import Path
import shutil
import statistics
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
BASELINE = {'ISS_RENDER_REUSE': '1', 'ISS_PARALLEL_RENDER': '1', 'ISS_DESCRIPTOR_FAST_PATH': '1',
            'ISS_GPU_PROFILE': '1', 'ISS_UPLOAD_WORKER': '0', 'ISS_BULK_COPY': '1',
            'ISS_CAPTURE_FRAMES': '0'}
METRICS = ('command_processor_ms', 'gpu_completed_work_ms', 'readback_stall_ms', 'shader_compile_ms',
           'pipeline_stall_ms', 'pipeline_skips', 'draws', 'dispatches', 'fault_reads', 'fault_writes',
           'protect_calls', 'page_protect_ms', 'buffer_upload_bytes', 'buffer_upload_ms',
           'recorder_work_ms', 'upload_worker_work_ms')


def parse_variant(text):
    label, _, spec = text.partition(':')
    env = {}
    for item in filter(None, spec.split(';')):
        key, _, value = item.partition('=')
        env[key] = value
    return label, env


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--rounds', type=int, default=1)
    ap.add_argument('--seconds', type=int, default=80)
    ap.add_argument('--seed', help='save seed directory (default out/user/savedata)')
    ap.add_argument('variants', nargs='+')
    a = ap.parse_args()
    variants = [parse_variant(v) for v in a.variants]
    destination = ROOT / 'out/benchmarks' / datetime.now().strftime('%Y%m%d-%H%M%S')
    destination.mkdir(parents=True)
    seed = destination / 'seed'
    shutil.copytree(Path(a.seed) if a.seed else ROOT / 'out/user/savedata', seed)
    report = {'variants': {label: env for label, env in variants}, 'baseline': BASELINE, 'runs': []}
    for round_index in range(a.rounds):
        for label, extra in variants:
            env = os.environ.copy()
            env.update(BASELINE)
            env.update(extra)
            env['ISS_SAVE_SEED'] = str(seed)
            log_path = destination / f'{label}-{round_index}.log'
            with log_path.open('w', encoding='utf-8') as log:
                subprocess.run([sys.executable, str(ROOT / 'scripts/run_probe.py'), '--linked', '--entry',
                                '--gpu', '--timeout', str(a.seconds)], env=env, stdout=log,
                               stderr=subprocess.STDOUT, check=False)
            run = json.loads((ROOT / 'out/latest-run.json').read_text(encoding='utf-8'))
            ok = (run.get('exit_code') == 0 and run.get('timeout') and not run.get('forced_stop') and
                  not run.get('events') and not run.get('io_summary', {}).get('failed_transfers'))
            rows = []
            with (Path(run['run_dir']) / 'frames.csv').open() as stream:
                for row in csv.DictReader(stream):
                    try:
                        rows.append({k: float(v) for k, v in row.items()})
                    except (ValueError, TypeError):
                        continue
            tail, duration = [], 0.0
            for row in reversed(rows):
                tail.append(row)
                duration += row['present_ms']
                if duration >= 30000:
                    break
            entry = {'label': label, 'round': round_index, 'run_dir': run['run_dir'], 'valid': bool(ok)}
            if ok and duration >= 30000:
                ordered = sorted(r['present_ms'] for r in tail)
                entry['tail_30s'] = {
                    'fps': len(tail) * 1000 / duration,
                    'p95_ms': ordered[int(len(ordered) * .95)],
                    'p99_ms': ordered[int(len(ordered) * .99)],
                    'mean': {k: statistics.mean(r.get(k, 0) for r in tail) for k in METRICS},
                }
            else:
                entry['valid'] = False
            report['runs'].append(entry)
            (destination / 'comparison.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
            summary = entry.get('tail_30s', {})
            print(f"{label} round {round_index}: valid={entry['valid']} fps={summary.get('fps', 0):.2f} "
                  f"cp={summary.get('mean', {}).get('command_processor_ms', 0):.2f} "
                  f"gpu={summary.get('mean', {}).get('gpu_completed_work_ms', 0):.2f} "
                  f"p95={summary.get('p95_ms', 0):.1f}", flush=True)
    means = {}
    for label, _ in variants:
        valid = [r for r in report['runs'] if r['label'] == label and r['valid']]
        if valid:
            means[label] = {'fps': statistics.mean(r['tail_30s']['fps'] for r in valid),
                            'p95_ms': statistics.mean(r['tail_30s']['p95_ms'] for r in valid),
                            'runs': len(valid),
                            **{k: statistics.mean(r['tail_30s']['mean'][k] for r in valid) for k in METRICS}}
    report['means'] = means
    report['note'] = ('Same save seed, last 30 s of each run; interleaved order. Not a simulation-speed '
                      'claim; no skipped work is permitted (pipeline_skips must stay 0).')
    (destination / 'comparison.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(json.dumps(means, indent=1))
    print(destination)


if __name__ == '__main__':
    main()
