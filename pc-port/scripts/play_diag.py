"""User-driven diagnostic session of the current experimental build (Claude, 2026-10-15).

The user plays normally; on exit a report is written (scripts/diag_report.py). Collected:
frames.csv (with GPU timestamps), per-thread CPU time, a sampling profile of the command
processor by source line (line-table DLL), and the cycle/census counters of the draw paths.

Isolation: build out/experimental/20261015-diag-session; saves are a private copy of the
claude-play saves inside the run folder (the play saves are never written); the shader cache is a
private copy of the claude-play lineage that is never written back. play2, releases, pack and the
trusted cache are only read.
"""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time
import json

import run_probe
from run_visual_regression import EXPERIMENTAL_OFF
from play_claude import FLAGS

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'out'
BUILD = OUT / 'experimental/20261015-diag-session'
PLAY = OUT / 'claude-play'
DIAG = {
    # Current experimental best: play flags + state switch, dynamic state ext, depth runs.
    'ISS_FAST_STATE_SWITCH': '1', 'ISS_FAST_DYN_EXT': '1',
    'ISS_NATIVE_DEPTH_RUN': '1', 'ISS_NATIVE_INSTANCED_FEATURE': '1',
    # Counters (rdtsc / small tables; printed to stderr periodically).
    'ISS_DRAW_CYCLES': '1', 'ISS_SWITCH_CYCLES': '1', 'ISS_DISPATCH_CYCLES': '1',
    'ISS_STAGE_MEMO_CYCLES': '1', 'ISS_DRAW_CENSUS': '1', 'ISS_OBTAIN_CENSUS': '1',
    'ISS_SEGMENT_CENSUS': '1',
    # GPU timestamps: frames.csv gets the GPU work time of each frame.
    'ISS_GPU_PROFILE': '1',
    # Passive or costly probes stay off.
    'ISS_ENGINE_STATS': '0', 'ISS_ENGINE_MARKERS': '0', 'ISS_HYBRID_FAMILY_PROFILE': '0',
    'ISS_NATIVE_ITEMS': '0', 'ISS_NATIVE_VERIFY': '0', 'ISS_NATIVE_DEPTH_RUN_VERIFY': '0',
    'ISS_NATIVE_DEPTH_RUN_DIAG': '0', 'ISS_CAPTURE_FRAMES': '0', 'ISS_DRAW_PIPE': '0',
    'ISS_HYBRID_BENCH_HIDDEN': '0',
}


def main():
    exe = BUILD / 'iss-probe.exe'
    dll = BUILD / 'iss-gpu.dll'
    if not exe.is_file() or not dll.is_file() or not dll.with_suffix('.pdb').is_file():
        sys.exit(f'Build de diagnóstico ausente: {BUILD}')
    tasks = subprocess.check_output(['tasklist', '/FI', 'IMAGENAME eq iss-probe.exe', '/FO', 'CSV'],
                                    text=True)
    if 'iss-probe.exe' in tasks.lower():
        sys.exit('O jogo já está aberto. Feche-o antes de iniciar o diagnóstico.')
    saves = PLAY / 'savedata'
    if not saves.exists():
        saves = OUT / 'user/city-savedata'
    lineage = PLAY / 'pipeline-cache/current'
    if not (lineage / 'CUSA00004/profile.bin').is_file():
        lineage = OUT / 'pipeline-cache-claude-dev/current'
    work = BUILD / 'cache-work' / time.strftime('%Y%m%d-%H%M%S')
    shutil.copytree(lineage, work)

    env = {k: v for k, v in os.environ.items() if not k.startswith(('ISS_', 'SHADPS4_', 'BB_'))}
    run_probe.apply_approved_environment(env, OUT)
    env.update({key: '0' for key in EXPERIMENTAL_OFF})
    env.update(FLAGS)
    env.update(DIAG)
    env.update(ISS_PROBE_EXE=str(exe), ISS_SAVE_SEED=str(saves),
               ISS_PIPELINE_CACHE_ROOT=str(work), ISS_MINIMAL_LOGS='0',
               ISS_DIAGNOSTIC_SESSION='1')
    os.environ.clear()
    os.environ.update(env)

    # The command processor sampler starts with the game and ends when its thread ends. Low rate
    # (about 400 samples/s) so the profile costs the command processor little.
    stamp = time.strftime('%Y%m%d-%H%M%S')
    profile = BUILD / f'cp-lines-{stamp}.json'
    sampler_log = BUILD / f'cp-lines-{stamp}.log'
    with sampler_log.open('w', encoding='utf-8') as log:
        sampler = subprocess.Popen(
            [sys.executable, str(ROOT / 'scripts/sample_cp.py'), str(profile), '--wait', '45',
             '--seconds', '86400', '--interval', '0.002', '--busy-min', '80', '--dll', str(dll),
             '--map', str(dll.with_suffix('.map'))],
            stdout=log, stderr=subprocess.STDOUT, cwd=ROOT,
            creationflags=subprocess.CREATE_NO_WINDOW)
        print('Diagnóstico iniciado. Jogue normalmente (de preferência na cidade, andando,'
              ' lutando e voando); feche o jogo quando quiser e o relatório será gerado.')
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
    (run_dir / 'diag-flags.json').write_text(json.dumps(DIAG, indent=2), encoding='utf-8')
    print('Gerando relatório...')
    subprocess.run([sys.executable, str(ROOT / 'scripts/diag_report.py'), str(run_dir)], cwd=ROOT)


if __name__ == '__main__':
    main()
