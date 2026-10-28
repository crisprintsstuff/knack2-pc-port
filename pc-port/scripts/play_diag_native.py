"""User-driven diagnostic session of the native renderer build (Claude, 2026-10-16).

The user plays (running through the city); nothing is changed in the game or the port. The run
uses the flags of the controlled benchmarks of out/experimental/20261015-native-materials (the
state of the native renderer work: unified memory, native materials, engine-thread recording
with chains) and collects frames.csv with GPU timestamps, per-thread CPU time and a sampling
profile of the command processor (function level, linker map). On exit the report is written
(scripts/diag_report.py).

Isolation: saves are a private copy inside the run folder; the shader cache is a private copy
of the shadow-stages seed, never written back. play2, releases, pack and the trusted cache are
only read.
"""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

import run_probe

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'out'
BUILD = OUT / 'experimental/20261015-native-materials/build'
SEED = OUT / 'experimental/20261015-shadow-stages/seed'
BASELINE = OUT / 'runs/20261015-151947-405784/controlled-benchmark.json'
NATIVE = {
    'ISS_UNIFIED_MEMORY': '1', 'ISS_UNIFIED_FLUSH_IRQ': '0', 'ISS_UNIFIED_FLUSH_US': '1000',
    'ISS_NATIVE_GBUFFER': '1', 'ISS_NATIVE_MATERIALS': '1', 'ISS_NATIVE_RECORDER': '1',
    'ISS_NATIVE_RECORDER_WRAP': '1', 'ISS_NATIVE_ALL_TECHNIQUES': '1',
    # GPU timestamps: frames.csv gets the GPU work time of each frame.
    'ISS_GPU_PROFILE': '1',
    'BB_VBLANK_HZ': '240', 'ISS_HYBRID_BENCH_HIDDEN': '0',
}


def main():
    exe = BUILD / 'iss-probe.exe'
    dll = BUILD / 'iss-gpu.dll'
    if not exe.is_file() or not dll.is_file():
        sys.exit(f'Build ausente: {BUILD}')
    tasks = subprocess.check_output(['tasklist', '/FI', 'IMAGENAME eq iss-probe.exe', '/FO', 'CSV'],
                                    text=True)
    if 'iss-probe.exe' in tasks.lower():
        sys.exit('O jogo já está aberto. Feche-o antes de iniciar o diagnóstico.')
    flags = json.loads(BASELINE.read_text(encoding='utf-8'))['flags']
    for key in ('ISS_PROBE_EXE', 'ISS_SAVE_ROOT', 'ISS_SAVE_SEED', 'ISS_PIPELINE_CACHE_ROOT',
                'ISS_PIPELINE_CACHE_SEED'):
        flags.pop(key, None)
    flags.update(NATIVE)

    saves = OUT / 'claude-play/savedata'
    if not saves.exists():
        saves = OUT / 'user/city-savedata'
    stamp = time.strftime('%Y%m%d-%H%M%S')
    work = BUILD.parent / 'diag-cache' / stamp
    shutil.copytree(SEED, work)

    env = {k: v for k, v in os.environ.items() if not k.startswith(('ISS_', 'SHADPS4_', 'BB_'))}
    run_probe.apply_approved_environment(env, OUT)
    env.update(flags)
    env.update(ISS_PROBE_EXE=str(exe), ISS_SAVE_SEED=str(saves),
               ISS_PIPELINE_CACHE_ROOT=str(work), ISS_MINIMAL_LOGS='0',
               ISS_DIAGNOSTIC_SESSION='1')
    os.environ.clear()
    os.environ.update(env)

    profile = BUILD.parent / f'diag-cp-{stamp}.json'
    sampler_log = BUILD.parent / f'diag-cp-{stamp}.log'
    with sampler_log.open('w', encoding='utf-8') as log:
        sampler = subprocess.Popen(
            [sys.executable, str(ROOT / 'scripts/sample_cp.py'), str(profile), '--wait', '45',
             '--seconds', '86400', '--interval', '0.002', '--busy-min', '80',
             '--map', str(dll.with_suffix('.map'))],
            stdout=log, stderr=subprocess.STDOUT, cwd=ROOT,
            creationflags=subprocess.CREATE_NO_WINDOW)
        print('Diagnóstico iniciado. Jogue normalmente na cidade (correndo); feche o jogo quando'
              ' quiser e o relatório será gerado.')
        try:
            run_probe.run(entry=True, linked=True, gpu=True, timeout=0, isolate_saves=True)
        finally:
            try:
                sampler.wait(timeout=600)
            except subprocess.TimeoutExpired:
                sampler.kill()
    record = json.loads((OUT / 'latest-run.json').read_text(encoding='utf-8'))
    run_dir = Path(record['run_dir'])
    if profile.is_file():
        shutil.copy2(profile, run_dir / 'cp-lines.json')
    shutil.copy2(sampler_log, run_dir / 'cp-lines.log')
    (run_dir / 'diag-flags.json').write_text(json.dumps(flags, indent=2), encoding='utf-8')
    print('Gerando relatório...')
    subprocess.run([sys.executable, str(ROOT / 'scripts/diag_report.py'), str(run_dir)], cwd=ROOT)


if __name__ == '__main__':
    main()
