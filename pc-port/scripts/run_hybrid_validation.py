"""Local, isolated 60-second hybrid checks; no release promotion or user-process termination."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
BASE = '''ISS_CLAMP_CACHE_FIRST=1 ISS_STAGE_MEMO=1 ISS_FAST_DRAW=1 ISS_FAST_REBIND=1
ISS_FAST_LEAN=1 ISS_FAST_SWITCH=1 ISS_SUBMIT_EVERY=1000 BB_VBLANK_HZ=240
ISS_COMPUTE_FAIRNESS=1 ISS_CP_CPU=14 ISS_STAGE_MEMO_LOOSE=1 ISS_STAGE_MEMO_TEXEL=1
ISS_LAZY_PIPELINES=1 ISS_SPEC_TRANSLATE=1 ISS_FAST_STATE_SWITCH=1 ISS_FAST_DYN_EXT=1
ISS_DRAW_PIPE=0 ISS_ENGINE_STATS=0
ISS_HYBRID_FAMILY_PROFILE=0 ISS_HYBRID_BENCH_HIDDEN=1 ISS_CAPTURE_FRAMES=0'''.split()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--mode', choices=('verify', 'visual-ab', 'visual-control', 'perf-ab'), required=True)
    parser.add_argument('--prefix', required=True)
    parser.add_argument('--frames', default='1200,1250,1300')
    parser.add_argument('--paired', action='store_true', help='Same-draw private-target GPU proof during verify')
    parser.add_argument('--visible', action='store_true',
                        help='Show the game window (perf/visual modes only; verification stays hidden)')
    args = parser.parse_args()
    if args.visible and (args.mode == 'verify' or args.paired):
        parser.error('Verification runs stay hidden (a visible verifier once blacked out the screen)')
    build = args.build.resolve()
    if not build.is_relative_to((ROOT / 'out/experimental').resolve()):
        parser.error('Use an isolated experimental build')
    hashes = {name: hashlib.sha256((build / name).read_bytes()).hexdigest()
              for name in ('iss-probe.exe', 'iss-gpu.dll')}
    cases = ([('verify1', 1, 1), ('verify2', 1, 1)] if args.mode == 'verify' else
             [('A2', 0, 0)] if args.mode == 'visual-control' else [('A', 0, 0), ('B', 1, 0)])
    reports = []
    for suffix, native, verify in cases:
        label = args.prefix + '-' + suffix
        log_path = build / (label + '.log')
        if log_path.exists():
            raise SystemExit('Refusing to overwrite prior evidence: ' + str(log_path))
        tasks = subprocess.check_output(['tasklist', '/FI', 'IMAGENAME eq iss-probe.exe', '/FO', 'CSV'], text=True)
        print(tasks, flush=True)
        if 'iss-probe.exe' in tasks.lower():
            raise SystemExit('Jogo já está rodando; aguardar o usuário, sem encerrá-lo.')
        base = [f for f in BASE if not (args.visible and f.startswith('ISS_HYBRID_BENCH_HIDDEN='))]
        if args.visible:
            base.append('ISS_HYBRID_BENCH_HIDDEN=0')
        flags = base + [f'ISS_ENGINE_MARKERS={native}', f'ISS_NATIVE_ITEM_RANGES={native}',
                        f'ISS_NATIVE_ITEMS={native}', f'ISS_NATIVE_VERIFY={verify}']
        if args.paired:
            if args.mode != 'verify':
                parser.error('--paired requires --mode verify')
            flags += ['ISS_NATIVE_VISUAL_PROBE=1',
                      'ISS_NATIVE_VISUAL_PROBE_DIR=' + str(build / (label + '-paired'))]
        if args.mode in ('visual-ab', 'visual-control'):
            flags += ['ISS_NATIVE_VISUAL_FRAMES=' + args.frames]
        command = [sys.executable, str(ROOT / 'scripts/run_controlled_benchmark.py'),
                   '--exe', str(build / 'iss-probe.exe'), '--seconds', '60', '--label', label,
                   '--cache-lineage', str(ROOT / 'out/pipeline-cache-claude-dev')]
        for flag in flags:
            command += ['--set', flag]
        (build / (label + '-command.json')).write_text(json.dumps(command, indent=2) + '\n')
        with log_path.open('w', encoding='utf-8') as log:
            result = subprocess.run(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
        active = json.loads((ROOT / 'out/latest-run.json').read_text(encoding='utf-8'))
        if Path(active['executable_path']).resolve() != build / 'iss-probe.exe':
            raise SystemExit('Latest run belongs to another executable; stop analysis.')
        folder = Path(active['run_dir'])
        benchmark = json.loads((folder / 'controlled-benchmark.json').read_text())
        stderr = (folder / 'stderr.log').read_text(errors='replace')
        mismatches = [line for line in stderr.splitlines() if 'NATIVE_VERIFY_MISMATCH' in line]
        stats = [line for line in stderr.splitlines() if line.startswith('NATIVE_ITEMS ')]
        last = stats[-1] if stats else ''
        verified = re.search(r'\bverified=(\d+)', last)
        native_draws = re.search(r'\breplays=(\d+)', last)
        clean_exit = (active.get('exit_code') == 0 and active.get('timeout') and
                      active.get('timeout_seconds') == 60 and not active.get('forced_stop') and
                      not active.get('corruption_dialog'))
        checks_passed = bool(clean_exit and not mismatches and verified and int(verified[1]) > 0
                             and native_draws and int(native_draws[1]) > 0)
        report = {'label': label, 'run_dir': str(folder), 'benchmark_valid': benchmark['valid'],
                  'command_exit': result.returncode, 'clean_exit': clean_exit,
                  'sampled_checks_passed': checks_passed if verify else None,
                  'mismatches': mismatches, 'last_native_stats': last,
                  'phase2_complete': False,
                  'note': 'Draw checks and visual equivalence are separate gates; no FPS gain is asserted.'}
        paired_pass = True
        if args.paired:
            paired_dir = build / (label + '-paired')
            # The log is authoritative for what the GPU callback concluded (incl. a failed write).
            pair_lines = {int(m['sample']): {k: int(v) for k, v in re.findall(r'(\w+)=(\d+)', m[0])}
                          for m in re.finditer(r'^NATIVE_VISUAL_PAIR sample=(?P<sample>\d+) .*$', stderr, re.M)}
            errors = [line for line in stderr.splitlines() if line.startswith('NATIVE_VISUAL_ERROR')]
            rejects = [line for line in stderr.splitlines() if line.startswith('NATIVE_VISUAL_REJECT')]
            samples = []
            for path in sorted(paired_dir.glob('sample-*.json')):
                sample = json.loads(path.read_text())
                sample['manifest'] = str(path)
                missing = [d['file'] for d in sample.get('dumps', [])
                           if not (paired_dir / d['file']).is_file() or
                           (paired_dir / d['file']).stat().st_size != d['bytes']]
                line = pair_lines.get(sample.get('sample'), {})
                sample['dumps_missing_or_short'] = missing
                sample['log_saved'] = line.get('saved')
                sample['files_ok'] = bool(sample.get('saved') and sample.get('mapped') and not missing and
                                          line.get('saved') == 1 and
                                          (sample.get('dumps') or not sample.get('nonempty')))
                samples.append(sample)
            meaningful = [x for x in samples if x.get('equal') and x.get('nonempty') and x['files_ok'] and
                          x.get('prepared_resources', 0) > 0 and x.get('previous_city_draws', 0) >= 7500]
            final_failed = max((v.get('failed', 0) for v in pair_lines.values()), default=0)
            paired_pass = (len(meaningful) >= 3 and all(x.get('equal') for x in samples) and
                           len(pair_lines) == len(samples) and not errors and final_failed == 0)
            report.update(paired_visual_passed=paired_pass, paired_samples=samples,
                          paired_log_lines=len(pair_lines), paired_errors=errors,
                          paired_failed_flag=final_failed, paired_rejects=rejects[-20:],
                          visual_scope='Same-draw color/depth planes from independent material plans; not whole-scene deterministic replay')
        reports.append(report)
        (build / (label + '-check.json')).write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
        if args.mode in ('visual-ab', 'visual-control'):
            frames = {ordinal: path.strip() for ordinal, path in
                      re.findall(r'^NATIVE_VISUAL_CAPTURE frame=(\d+) path=(.+)$', stderr, re.M)}
            manifest = {'frames': frames, 'provenance': {'build_hashes': hashes, 'run_dir': str(folder)},
                        'requested_frames': args.frames.split(',')}
            (build / (label + '-visual.json')).write_text(json.dumps(manifest, indent=2) + '\n')
            if set(frames) != set(args.frames.split(',')):
                raise SystemExit('Requested visual frames were not all captured; no equality conclusion.')
        print(json.dumps(report, ensure_ascii=False), flush=True)
        if not clean_exit or mismatches or (verify and not checks_passed) or not paired_pass:
            raise SystemExit('Verification requires repair; not starting the next case.')
    (build / (args.prefix + '-summary.json')).write_text(json.dumps(reports, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
