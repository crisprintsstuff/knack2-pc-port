"""Visible, isolated shadow stage runs with an explicit baseline and immutable seed."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
WORK = ROOT / 'out/experimental/20261015-shadow-stages'

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('build', type=Path)
    ap.add_argument('label')
    ap.add_argument('--seconds', type=int, default=120)
    ap.add_argument('--set', action='append', default=[])
    ap.add_argument('--profile', action='store_true')
    ap.add_argument('--save-seed', type=Path, help='Save folder to start from (default: the city saves)')
    ap.add_argument('--cache-seed', type=Path, help='Pipeline cache generation to start from (a folder with CUSA00004/profile.bin; default: the shadow-stages seed)')
    args = ap.parse_args()
    build = args.build.resolve()
    if not build.is_relative_to((ROOT / 'out/experimental').resolve()):
        ap.error('experimental builds only')
    if any(c not in 'abcdefghijklmnopqrstuvwxyz0123456789-_' for c in args.label):
        ap.error('label must be lowercase letters, numbers, hyphen or underscore')
    tasks = subprocess.check_output(['tasklist', '/FI', 'IMAGENAME eq iss-probe.exe', '/FO', 'CSV'], text=True)
    if 'iss-probe.exe' in tasks.lower():
        raise SystemExit('Game already running; leave it alone.')
    evidence = WORK / 'runs' / args.label
    evidence.mkdir(parents=True, exist_ok=False)
    source = ROOT / 'out/runs/20261015-151947-405784/controlled-benchmark.json'
    flags = json.loads(source.read_text())['flags']
    for k in ('ISS_PROBE_EXE', 'ISS_SAVE_ROOT', 'ISS_SAVE_SEED', 'ISS_PIPELINE_CACHE_ROOT', 'ISS_PIPELINE_CACHE_SEED'):
        flags.pop(k, None)
    flags.update(BB_VBLANK_HZ='240', ISS_HYBRID_BENCH_HIDDEN='0',
                 ISS_NATIVE_SHADOW_STATS='0', ISS_NATIVE_SHADOW_COST='0',
                 ISS_NATIVE_SHADOW_VERIFY='0',
                 ISS_NATIVE_SHADOW_VERIFY_DIR='experimental/20261015-shadow-stages/runs/' + args.label + '/verify')
    for item in args.set:
        k, sep, v = item.partition('=')
        if not sep: ap.error('expected KEY=VALUE')
        flags[k] = v
    if flags['BB_VBLANK_HZ'] != '240' or flags['ISS_HYBRID_BENCH_HIDDEN'] != '0':
        ap.error('240 Hz and visible window are required')
    seed = args.cache_seed.resolve() if args.cache_seed else WORK / 'seed'
    if not seed.exists():
        shutil.copytree(ROOT / 'out/pipeline-cache-claude-dev/current', seed)
    shutil.copytree(seed, evidence / 'cache/current')
    cmd = [sys.executable, str(ROOT / 'scripts/run_controlled_benchmark.py'),
           '--exe', str(build / 'iss-probe.exe'), '--seconds', str(args.seconds),
           '--label', args.label, '--cache-lineage', str(evidence / 'cache')]
    for k, v in flags.items(): cmd += ['--set', k + '=' + v]
    if args.profile: cmd.append('--profile')
    if args.save_seed: cmd += ['--save-seed', str(args.save_seed.resolve())]
    manifest = dict(command=cmd, flags=flags,
                    cache_seed_sha256=hashlib.sha256((seed / 'CUSA00004/profile.bin').read_bytes()).hexdigest(),
                    baseline_report=str(source))
    (evidence / 'command.json').write_text(json.dumps(manifest, indent=2))
    with (evidence / 'console.log').open('w') as log:
        result = subprocess.call(cmd, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
    record = json.loads((ROOT / 'out/latest-run.json').read_text())
    (evidence / 'run-pointer.json').write_text(json.dumps(record, indent=2))
    print(args.label, record['run_dir'], 'exit=', result, flush=True)
    return result

if __name__ == '__main__':
    raise SystemExit(main())
