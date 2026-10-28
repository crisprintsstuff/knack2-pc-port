"""User-driven session of the v1 best native build with the global pause guard (Claude, 2026-10-17).

Build: out/experimental/20261016-native-passes-v1/best-pause-guard-blackbox (dfa773c, ~59 FPS
standing, plus gpu/pause_guard.h: in the pause and 3 s after it every native path is off, plus the
texture cache double free fix, plus the GPU black box gpu/gpu_blackbox.h). Flags as in
test-v1-guard.sh, without the pad script. On exit the black box report is printed
(scripts/gpu_crash_report.py).

Saves are the user's real city saves (out/user/city-savedata, backup in city-savedata-backup-20261017):
what the user saves here is what the scripted runs load. The GPU black box stays on, so a hang names
the command.

Shader cache (2026-10-18, accumulating): %LOCALAPPDATA%/iss-port/shader-cache/current starts as the shadow-stages
seed and grows with every session: each session plays on a copy of it, and after a clean exit (the
port's own shutdown, no GPU crash, a real session) that copy becomes the new current, the old one
is kept as previous. A shader compiled once is never compiled again on this machine (the driver's
cache file inside it grows too). A new generation is marked unverified until a session has loaded
it and played: if that session dies before playing (a recipe written near shutdown once crashed the
next preload), the previous generation comes back.
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
BUILD = OUT / 'experimental/20261016-native-passes-v1/best-pause-guard-blackbox'
SEED = OUT / 'experimental/20261015-shadow-stages/seed'
BASELINE = OUT / 'runs/20261015-151947-405784/controlled-benchmark.json'
SAVES = OUT / 'user/city-savedata'
# Outside OneDrive: files just copied into a OneDrive folder take ~5.5 ms each to open while the
# sync client processes them (0.2 ms afterwards), which made the shader preload of every launch
# 20-27 s (4200 pipelines; preload-prof2 2026-10-18: 96% of it in IOFile::Open).
LOCAL = Path(os.environ.get('LOCALAPPDATA', str(OUT / 'user'))) / 'iss-port'
LINEAGE = LOCAL / 'shader-cache'
# Shader library (2026-10-18): the recipes the current build compiled in every scene save (city,
# Visitors, Aftermath, Parting, Catching Smoke, Chasing the Light), 11 157 pipelines, preloaded
# before the game starts (first launch on a machine ~40 s, then ~12 s). Its permutation indexes
# are its own lineage: the accumulated cache then starts empty on top of it.
LIBRARY = LOCAL / 'library-v2'
LIBRARY_SOURCE = OUT / 'experimental/20261016-native-passes-v1/shaders/library-v2/library-selected'
MIN_FRAMES = 600  # a session that played (not only the preload and the title screen)
NATIVE = {
    'ISS_UNIFIED_MEMORY': '1', 'ISS_UNIFIED_FLUSH_IRQ': '0', 'ISS_UNIFIED_FLUSH_US': '1000',
    'ISS_NATIVE_MATERIALS': '1', 'ISS_NATIVE_GBUFFER_SETS': '1',
    'ISS_NATIVE_PASSES': '1', 'ISS_NATIVE_PASSES_SUBMIT': '1', 'ISS_NATIVE_PASSES_DYNAMIC': '1',
    'ISS_NATIVE_SHADOW': '0', 'ISS_NATIVE_DEPTH_RUN': '0',
    'ISS_GPU_PROFILE': '0', 'ISS_GPU_BLACKBOX': '1', 'ISS_INDIRECT_GUARD': '0',
    # One test report at 90 s (gpu-blackbox-test.txt), to see the black box without a hang.
    'ISS_GPU_BLACKBOX_TEST_S': '90',
    'BB_VBLANK_HZ': '240', 'ISS_HYBRID_BENCH_HIDDEN': '0',
}


def library():
    """The shader library folder (copied out of OneDrive once), or None."""
    if not (LIBRARY / 'manifest.json').is_file() and (LIBRARY_SOURCE / 'manifest.json').is_file():
        print('Copiando a biblioteca de shaders para', LIBRARY, '(uma vez)...')
        shutil.copytree(LIBRARY_SOURCE, LIBRARY, dirs_exist_ok=True)
    return LIBRARY if (LIBRARY / 'manifest.json').is_file() else None


def prepare_cache(lib=None):
    """The session's copy of the accumulated shader cache."""
    current = LINEAGE / 'current'
    tag = 'library ' + (lib / 'manifest.json').read_bytes()[-64:].hex() if lib else 'seed'
    tag_file = LINEAGE / 'lineage.txt'
    if current.is_dir() and (not tag_file.is_file() or tag_file.read_text(encoding='utf-8') != tag):
        # Another base (seed or another library): its permutation indexes would conflict.
        old = LINEAGE / time.strftime('old-%Y%m%d-%H%M%S')
        shutil.move(str(current), str(old))
        print('Cache acumulado de outra base guardado em', old)
    if not (current / 'CUSA00004/profile.bin').is_file():
        LINEAGE.mkdir(parents=True, exist_ok=True)
        if current.exists():
            shutil.rmtree(current)
        if lib:
            (current / 'CUSA00004').mkdir(parents=True)
            shutil.copy2(SEED / 'CUSA00004/profile.bin', current / 'CUSA00004/profile.bin')
        else:
            shutil.copytree(SEED, current)
        tag_file.write_text(tag, encoding='utf-8')
        print('Cache de shaders acumulado criado:', current)
    sessions = LOCAL / 'play-cache'
    sessions.mkdir(parents=True, exist_ok=True)
    for old in sorted(p for p in sessions.iterdir() if p.is_dir())[:-3]:
        shutil.rmtree(old, ignore_errors=True)  # keep the last sessions' copies only
    work = sessions / time.strftime('%Y%m%d-%H%M%S')
    while work.exists():
        work = work.with_name(work.name + 'b')
    shutil.copytree(current, work)
    (work / '.unverified').unlink(missing_ok=True)
    return work


def count_files(path):
    return sum(1 for f in path.rglob('*') if f.is_file() and f.name != '.unverified')


def finish_cache(work, run_dir):
    """Promotes the session's cache after a clean session; rolls back an unverified one that failed."""
    current, previous = LINEAGE / 'current', LINEAGE / 'previous'
    marker = current / '.unverified'
    frames = 0
    if (run_dir / 'frames.csv').is_file():
        frames = max(0, sum(1 for _ in open(run_dir / 'frames.csv', encoding='utf-8', errors='ignore')) - 1)
    stderr = (run_dir / 'stderr.log').read_text(encoding='utf-8', errors='ignore') if (run_dir / 'stderr.log').is_file() else ''
    crashed = (run_dir / 'gpu-crash.json').is_file() or (run_dir / 'gpu-crash.txt').is_file()
    played = frames >= MIN_FRAMES
    if marker.is_file():
        if played:
            marker.unlink()
        elif previous.is_dir():
            print('A sessão com o cache novo terminou antes de jogar: voltando à geração anterior.')
            shutil.rmtree(current)
            shutil.move(str(previous), str(current))
            shutil.rmtree(work, ignore_errors=True)
            return
    clean = played and not crashed and 'PORT_STOP' in stderr
    if not clean:
        print(f'Cache de shaders não promovido (frames={frames}, crash={crashed}).')
        return
    if not (work / 'CUSA00004/profile.bin').is_file() or count_files(work) <= count_files(current):
        print('Nenhum shader novo nesta sessão; cache acumulado mantido.')
        shutil.rmtree(work, ignore_errors=True)
        return
    if previous.exists():
        shutil.rmtree(previous)
    shutil.move(str(current), str(previous))
    shutil.move(str(work), str(current))
    (current / '.unverified').write_text(str(run_dir), encoding='utf-8')
    print(f'Cache de shaders acumulado: {count_files(current)} arquivos (antes {count_files(previous)}).')


def watch_preload(started, work):
    """The launcher's shader loader: a progress bar while the game preloads its pipelines."""
    import re
    run_dir = None
    while run_dir is None:
        time.sleep(0.5)
        runs = [p for p in (OUT / 'runs').iterdir() if p.is_dir() and p.stat().st_ctime >= started - 5]
        if runs:
            run_dir = max(runs, key=lambda p: p.stat().st_ctime)
    log = run_dir / 'stderr.log'
    own_keys = sum(1 for _ in (work / 'CUSA00004').glob('*.key'))
    total = None
    offset = 0
    last = -1
    begun = time.time()
    while time.time() - begun < 900:
        time.sleep(0.5)
        if not log.is_file():
            continue
        with open(log, encoding='utf-8', errors='ignore') as f:
            f.seek(offset)
            chunk = f.read()
            offset = f.tell()
        for line in chunk.splitlines():
            m = re.search(r'PIPELINE_LIBRARY ready=1 .*additional_keys=(\d+)', line)
            if m:
                total = int(m.group(1)) + own_keys
            m = re.search(r'Pipeline preload: (\d+) entries', line)
            if m and total:
                done = min(int(m.group(1)), total)
                if done != last:
                    last = done
                    bar = '#' * (30 * done // total)
                    print(f'\rPreparando shaders [{bar:<30}] {done}/{total}', end='', flush=True)
            m = re.search(r'Pipeline preload finished in ([\d.]+) s', line)
            if m:
                if total:
                    print(f'\rPreparando shaders [{"#" * 30}] {total}/{total}')
                print(f'Shaders prontos em {float(m.group(1)):.0f} s.')
                return


def main():
    exe = BUILD / 'iss-probe.exe'
    if not exe.is_file() or not (BUILD / 'iss-gpu.dll').is_file():
        sys.exit(f'Build ausente: {BUILD}')
    tasks = subprocess.check_output(['tasklist', '/FI', 'IMAGENAME eq iss-probe.exe', '/FO', 'CSV'],
                                    text=True)
    if 'iss-probe.exe' in tasks.lower():
        sys.exit('O jogo já está aberto. Feche-o antes.')
    flags = json.loads(BASELINE.read_text(encoding='utf-8'))['flags']
    for key in ('ISS_PROBE_EXE', 'ISS_SAVE_ROOT', 'ISS_SAVE_SEED', 'ISS_PIPELINE_CACHE_ROOT',
                'ISS_PIPELINE_CACHE_SEED'):
        flags.pop(key, None)
    flags.update(NATIVE)

    lib = library()
    work = prepare_cache(lib)
    env = {k: v for k, v in os.environ.items() if not k.startswith(('ISS_', 'SHADPS4_', 'BB_'))}
    run_probe.apply_approved_environment(env, OUT)
    env.update(flags)
    env.update(ISS_PROBE_EXE=str(exe), ISS_SAVE_ROOT=str(SAVES), ISS_SAVE_SEED=str(SAVES),
               ISS_PIPELINE_CACHE_ROOT=str(work), ISS_MINIMAL_LOGS='0')
    if lib:
        env['ISS_PIPELINE_LIBRARY'] = str(lib)
    os.environ.clear()
    os.environ.update(env)
    print('Jogo iniciado (v1 + trava global do pause + caixa-preta da GPU). Feche a janela para encerrar.')
    import threading
    threading.Thread(target=watch_preload, args=(time.time(), work), daemon=True).start()
    try:
        run_probe.run(entry=True, linked=True, gpu=True, timeout=0, isolate_saves=False)
    except Exception as error:  # a GPU crash ends the game: the report still runs
        print('O jogo terminou com erro:', error)
    record = json.loads((OUT / 'latest-run.json').read_text(encoding='utf-8'))
    run_dir = Path(record['run_dir'])
    (run_dir / 'play-flags.json').write_text(json.dumps(flags, indent=2), encoding='utf-8')
    print('Run:', run_dir)
    subprocess.run([sys.executable, str(ROOT / 'scripts/gpu_crash_report.py'), str(run_dir)], cwd=ROOT)
    try:
        finish_cache(work, run_dir)
    except Exception as error:  # the session itself is done; never lose it over the cache
        print('Falha ao acumular o cache de shaders:', error)


if __name__ == '__main__':
    main()
