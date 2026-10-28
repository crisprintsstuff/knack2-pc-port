"""Visible 60-second benchmarks of several experimental builds with identical flags, in sequence.

Usage: python scripts/run_regress_check.py --prefix NAME [--extra FLAG=V ...] BUILD_DIR [BUILD_DIR ...]
Uses the play_claude.py flag set (what the user plays with), cache lineage copies, refuses to
run while the user's game is open, and never overwrites earlier evidence.
"""
import argparse
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from play_claude import FLAGS  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('builds', nargs='+', type=Path)
    ap.add_argument('--prefix', required=True)
    ap.add_argument('--extra', action='append', default=[])
    ap.add_argument('--hidden', action='store_true')
    ap.add_argument('--seconds', type=int, default=60)
    args = ap.parse_args()
    flags = [f'{k}={v}' for k, v in FLAGS.items()] + [
        'ISS_DRAW_PIPE=0', 'ISS_ENGINE_STATS=0', 'ISS_HYBRID_FAMILY_PROFILE=0', 'ISS_CAPTURE_FRAMES=0',
        'ISS_ENGINE_MARKERS=0', 'ISS_NATIVE_ITEM_RANGES=0', 'ISS_NATIVE_ITEMS=0', 'ISS_NATIVE_VERIFY=0',
        f'ISS_HYBRID_BENCH_HIDDEN={1 if args.hidden else 0}'] + args.extra
    for build in args.builds:
        build = build.resolve()
        if not build.is_relative_to((ROOT / 'out/experimental').resolve()):
            ap.error('Use an isolated experimental build')
        label = f'{args.prefix}-{build.name}'
        log = build / f'{label}.log'
        if log.exists():
            raise SystemExit('Refusing to overwrite prior evidence: ' + str(log))
        tasks = subprocess.check_output(['tasklist', '/FI', 'IMAGENAME eq iss-probe.exe', '/FO', 'CSV'], text=True)
        if 'iss-probe.exe' in tasks.lower():
            raise SystemExit('Jogo já está rodando; aguardar o usuário, sem encerrá-lo.')
        cmd = [sys.executable, str(ROOT / 'scripts/run_controlled_benchmark.py'), '--exe', str(build / 'iss-probe.exe'),
               '--seconds', str(args.seconds), '--label', label, '--cache-lineage', str(ROOT / 'out/pipeline-cache-claude-dev')]
        for f in flags:
            cmd += ['--set', f]
        (build / f'{label}-command.json').write_text(json.dumps(cmd, indent=2) + '\n')
        with log.open('w', encoding='utf-8') as out:
            subprocess.run(cmd, cwd=ROOT, stdout=out, stderr=subprocess.STDOUT)
        active = json.loads((ROOT / 'out/latest-run.json').read_text(encoding='utf-8'))
        bench = json.loads((Path(active['run_dir']) / 'controlled-benchmark.json').read_text())
        print(label, active['run_dir'], 'valid=', bench.get('valid'), 'fps=', bench.get('fps'), flush=True)


if __name__ == '__main__':
    main()
