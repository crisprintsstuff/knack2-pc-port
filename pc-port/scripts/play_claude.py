"""Play with the current Claude build: persistent saves and a pipeline cache that grows.

The build, saves and cache live apart from the approved launcher (out/claude-play). The cache
is a lineage: each session runs on a copy of out/claude-play/pipeline-cache/current, which
replaces it only after a clean exit (a crash never damages the cache the next session uses).
"""
import os
from pathlib import Path
import shutil
import sys
import time
import json

import run_probe
from run_visual_regression import EXPERIMENTAL_OFF

OUT = Path(__file__).resolve().parents[1] / 'out'
BUILD = OUT / 'releases/20261015-play2'
PLAY = OUT / 'claude-play'
FLAGS = {
    # CPU / command processor (measured best, 2026-10-14).
    'ISS_CLAMP_CACHE_FIRST': '1', 'ISS_STAGE_MEMO': '1', 'ISS_FAST_DRAW': '1',
    'ISS_FAST_REBIND': '1', 'ISS_FAST_LEAN': '1', 'ISS_FAST_SWITCH': '1',
    'ISS_SUBMIT_EVERY': '1000', 'BB_VBLANK_HZ': '240', 'ISS_COMPUTE_FAIRNESS': '1',
    'ISS_CP_CPU': '14', 'ISS_STAGE_MEMO_LOOSE': '1', 'ISS_STAGE_MEMO_TEXEL': '1',
    # New pipelines compile in parallel; the command processor does not wait for them.
    'ISS_LAZY_PIPELINES': '1',
    # New shaders met by the scout are translated ahead by worker threads.
    'ISS_SPEC_TRANSLATE': '1',
}


def main():
    exe = BUILD / 'iss-probe.exe'
    if not exe.is_file() or not exe.with_name('iss-gpu.dll').is_file():
        sys.exit(f'Missing build: {BUILD}')
    saves = PLAY / 'savedata'
    if not saves.exists():
        shutil.copytree(OUT / 'user/city-savedata', saves)
    lineage = PLAY / 'pipeline-cache'
    if not (lineage / 'current/CUSA00004/profile.bin').is_file():
        shutil.copytree(OUT / 'pipeline-cache-claude-dev/current', lineage / 'current')
    work = PLAY / 'cache-work' / time.strftime('%Y%m%d-%H%M%S')
    shutil.copytree(lineage / 'current', work)

    env = {k: v for k, v in os.environ.items() if not k.startswith(('ISS_', 'SHADPS4_', 'BB_'))}
    run_probe.apply_approved_environment(env, OUT)
    env.update({key: '0' for key in EXPERIMENTAL_OFF})
    env.update(FLAGS)
    env.update(ISS_PROBE_EXE=str(exe), ISS_SAVE_ROOT=str(saves),
               ISS_PIPELINE_CACHE_ROOT=str(work), ISS_MINIMAL_LOGS='0')
    os.environ.clear()
    os.environ.update(env)
    run_probe.run(entry=True, linked=True, gpu=True, timeout=0)

    record = json.loads((OUT / 'latest-run.json').read_text(encoding='utf-8'))
    stderr = Path(record['run_dir']) / 'stderr.log'
    text = stderr.read_text(encoding='utf-8', errors='replace') if stderr.is_file() else ''
    if record.get('exit_code') == 0 and not record.get('corruption_dialog') and \
            'WarmUp: Preloaded' in text:
        previous = lineage / ('gen-' + time.strftime('%Y%m%d-%H%M%S'))
        (lineage / 'current').rename(previous)
        shutil.move(str(work), str(lineage / 'current'))
        for old in sorted(lineage.glob('gen-*'))[:-3]:
            shutil.rmtree(old)
        print('Shader cache saved for the next session.')
    else:
        print(f'Session did not end cleanly: the shader cache was kept as before ({work}).')


if __name__ == '__main__':
    main()
