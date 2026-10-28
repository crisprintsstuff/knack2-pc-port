"""Measure a finite isolated city run with an explicit EXE/DLL and clean flags."""
import argparse
import csv
import json
import shutil
import os
from pathlib import Path
import statistics
import subprocess
import sys
import threading
import time

import run_probe
from run_visual_regression import EXPERIMENTAL_OFF

OUT = Path(__file__).resolve().parents[1] / 'out'


def summarize(folder, seconds=30):
    rows = []
    with (folder / 'frames.csv').open(encoding='utf-8') as stream:
        for row in csv.DictReader(stream):
            try:
                rows.append({k: float(v) for k, v in row.items()})
            except (TypeError, ValueError):
                continue
    tail, elapsed = [], 0
    for row in reversed(rows):
        tail.append(row)
        elapsed += row['present_ms']
        if elapsed >= seconds * 1000:
            break
    if not tail:
        return {'valid': False, 'reason': 'no completed frame records'}
    values = sorted(row['present_ms'] for row in tail)
    return {
        'valid_window': elapsed >= seconds * 1000,
        'frames': len(tail), 'duration_seconds': elapsed / 1000,
        'fps': len(tail) * 1000 / elapsed,
        'p50_ms': statistics.median(values),
        'p95_ms': values[min(len(values) - 1, int(len(values) * .95))],
        'p99_ms': values[min(len(values) - 1, int(len(values) * .99))],
        'max_ms': max(values),
        'over_16_667ms': sum(v > 1000 / 60 for v in values),
        'over_33_333ms': sum(v > 1000 / 30 for v in values),
        'over_50ms': sum(v > 50 for v in values),
        'mean': {key: statistics.mean(row.get(key, 0) for row in tail)
                 for key in tail[0] if key != 'frame'},
        'total_frames': len(rows),
        'total_pipeline_skips': sum(row.get('pipeline_skips', 0) for row in rows),
        'total_compile_ms': sum(row.get('shader_compile_ms', 0) for row in rows),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--label', required=True)
    parser.add_argument('--seconds', type=int, default=140)
    parser.add_argument('--set', action='append', default=[])
    parser.add_argument('--profile', action='store_true', help='Sample the command processor after warmup')
    parser.add_argument('--cache-lineage', type=Path,
                        help='Pipeline cache lineage: the run starts from DIR/current; a clean exit makes its cache the next generation')
    parser.add_argument('--profile-threads', type=int, default=0, help='With --profile: sample the N busiest threads instead of the command processor')
    parser.add_argument('--save-seed', type=Path, default=OUT / 'user/city-savedata',
                        help='Save folder the run starts from (copied: never written)')
    args = parser.parse_args()
    if args.seconds < 60:
        parser.error('Use at least 60 seconds for warmup and a 30-second tail')
    exe = args.exe.resolve()
    for name in (exe, exe.with_name('iss-gpu.dll')):
        if not name.is_file():
            parser.error(f'Missing runtime: {name}')
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(('ISS_', 'SHADPS4_', 'BB_'))}
    run_probe.apply_approved_environment(env, OUT)
    env.update({key: '0' for key in EXPERIMENTAL_OFF})
    env.update(ISS_PROBE_EXE=str(exe), ISS_SAVE_SEED=str(args.save_seed.resolve()),
               ISS_MINIMAL_LOGS='0')
    for item in args.set:
        key, sep, value = item.partition('=')
        if not sep or not key.startswith(('ISS_', 'SHADPS4_', 'BB_')):
            parser.error(f'Expected ISS_/SHADPS4_ variable=value: {item}')
        if key in ('ISS_PROBE_EXE', 'ISS_SAVE_ROOT', 'ISS_SAVE_SEED',
                   'ISS_PIPELINE_CACHE_ROOT', 'ISS_PIPELINE_CACHE_SEED'):
            parser.error(f'Isolation setting is not an experiment flag: {key}')
        env[key] = value
    if env.get('ISS_NATIVE_CPU_DRAWS') == '1':
        from run_cpu_native import verify_batch_shaders
        verify_batch_shaders(OUT)
        env['ISS_CPU_BUNDLE_SPV_VERIFIED'] = '1'
    lineage_work = None
    if args.cache_lineage:
        # Each run starts from the newest generation (permutation indices stay one lineage) and
        # writes a work copy; the trusted seed and the launcher caches are never touched.
        current = args.cache_lineage / 'current'
        if not (current / 'CUSA00004' / 'profile.bin').is_file():
            parser.error(f'No cache generation at {current}')
        # Outside OneDrive: freshly copied files there take ~5.5 ms each to open (the sync client),
        # which made the preload 20-27 s.
        lineage_work = (Path(os.environ.get('LOCALAPPDATA', str(OUT))) / 'iss-port/claude-cache-work'
                        / time.strftime('%Y%m%d-%H%M%S'))
        shutil.copytree(current, lineage_work)
        env['ISS_PIPELINE_CACHE_ROOT'] = str(lineage_work)
    os.environ.clear()
    os.environ.update(env)
    stop = threading.Event()
    monitor = None
    started = time.time()
    if args.profile:
        from run_cpu_command_profile import tail_frames
        def sample():
            while not stop.wait(2):
                try:
                    active = json.loads((OUT / 'latest-run.json').read_text())
                    folder = Path(active['run_dir'])
                    if Path(active['executable_path']) != exe or not active.get('pid'):
                        continue
                    if (folder / 'run.json').stat().st_mtime < started:
                        continue
                    rows = tail_frames(folder / 'frames.csv')
                    if len(rows) < 180 or rows[-1]['frame'] < 1200:
                        continue
                    if sum(r['pipeline_stall_ms'] for r in rows) > 1 or \
                            sum(r['draws'] for r in rows) / len(rows) < 7500:
                        continue
                    with (folder / 'cpu-profile-console.log').open('w', encoding='utf-8') as log:
                        subprocess.run([sys.executable, str(OUT.parent / 'scripts/profile_threads.py'),
                            '--pid', str(active['pid']),
                            *(['--threads', str(args.profile_threads)] if args.profile_threads else
                              ['--thread-name', 'GpuCommandProcessor', '--threads', '1']),
                            '--hz', '250', '--seconds', '20',
                            '--map', str(exe.with_name('iss-gpu.map')),
                            '--out', str(folder / 'cpu-command-profile.json')],
                            stdout=log, stderr=subprocess.STDOUT, timeout=35)
                    return
                except (OSError, ValueError, KeyError):
                    continue
                except subprocess.TimeoutExpired:
                    return
        monitor = threading.Thread(target=sample, daemon=True)
        monitor.start()
    try:
        result = run_probe.run(entry=True, linked=True, gpu=True, timeout=args.seconds)
    finally:
        stop.set()
        if monitor:
            monitor.join(timeout=40)
    record = json.loads((OUT / 'latest-run.json').read_text(encoding='utf-8'))
    folder = Path(record['run_dir'])
    if lineage_work:
        stderr_text = (folder / 'stderr.log').read_text(encoding='utf-8', errors='replace')
        if (record.get('exit_code') == 0 and not record.get('corruption_dialog') and
                'WarmUp: Preloaded' in stderr_text):
            generations = args.cache_lineage
            previous = generations / ('gen-' + time.strftime('%Y%m%d-%H%M%S'))
            (generations / 'current').rename(previous)
            shutil.move(str(lineage_work), str(generations / 'current'))
            for old in sorted(generations.glob('gen-*'))[:-3]:
                shutil.rmtree(old)
            print(f'CACHE_LINEAGE promoted {lineage_work.name}; previous kept as {previous.name}')
        else:
            print(f'CACHE_LINEAGE kept current; work copy left at {lineage_work}')
    try:
        metrics = summarize(folder)
    except (OSError, ValueError) as error:
        metrics = {'valid': False, 'reason': str(error)}
    valid = (record.get('exit_code') == 0 and record.get('timeout') and
             not record.get('forced_stop') and not record.get('corruption_dialog') and
             metrics.get('valid_window') and metrics.get('total_pipeline_skips') == 0)
    report = {'label': args.label, 'run_dir': str(folder), 'valid': bool(valid),
              'exe_sha256': record['executable_sha256'],
              'gpu_sha256': record['gpu_sha256'],
              'flags': {k: v for k, v in env.items() if k.startswith(('ISS_', 'SHADPS4_', 'BB_'))},
              'tail_30s': metrics,
              'note': 'Guest flips; CPU/GPU counters overlap. Scene may vary after loading.'}
    (folder / 'controlled-benchmark.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({k: report[k] for k in ('label', 'run_dir', 'valid')}))
    print(json.dumps({k: v for k, v in metrics.items() if k != 'mean'}))
    return 0 if valid else result or 1


if __name__ == '__main__':
    raise SystemExit(main())
