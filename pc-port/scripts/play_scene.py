"""User-driven session of the main base (porte-m) from one of the user's separate saves
(Claude, 2026-10-17). No pad script: the user plays. The save is a fresh copy per session
(out/user/scenes/<scene> -> <build>/play-saves/<scene>-<time>), so SaveBackup and the main city
save are never written. At the end: the black box report and the frame summary.

    python play_scene.py <visitors|aftermath|parting|catching-smoke|chasing-light>
"""
from pathlib import Path
import shutil
import sys
import time

import play_porte  # noqa: F401  (sets the porte-m build and its flags on play_v1_guard)
import play_v1_guard as play

SCENES = {
    'visitors': 'The Visitors (2%)',
    'aftermath': 'Aftermath (4%)',
    'parting': 'Parting of the Ways (6%)',
    'catching-smoke': 'Catching Smoke (10%)',
    'chasing-light': 'Chasing the Light (17%)',
}


def main():
    if len(sys.argv) != 2 or sys.argv[1] not in SCENES:
        sys.exit('Cena: ' + ', '.join(SCENES))
    scene = sys.argv[1]
    source = play.OUT / 'user/scenes' / scene
    if not source.is_dir():
        sys.exit(f'Save ausente: {source}')
    work = play.BUILD / 'play-saves' / f'{scene}-{time.strftime("%Y%m%d-%H%M%S")}'
    shutil.copytree(source, work)
    play.SAVES = work
    print(f'Cena: {SCENES[scene]} (copia do save em {work})')
    play.main()
    run_dir = Path(__import__('json').loads(
        (play.OUT / 'latest-run.json').read_text(encoding='utf-8'))['run_dir'])
    summarize(run_dir / 'frames.csv')


def summarize(path):
    """Per minute of play: FPS, 1% low, frames over 33 ms and the command processor's time."""
    import csv
    if not path.is_file():
        return
    rows = list(csv.DictReader(path.open(encoding='utf-8')))
    minutes, t = {}, 0.0
    for r in rows:
        ms = float(r['present_ms'])
        t += ms / 1000
        minutes.setdefault(int(t // 60), []).append((ms, float(r['command_processor_ms'])))
    print()
    print('minuto | FPS  | 1% low | >33 ms | CP ms')
    for m, fr in sorted(minutes.items()):
        ms = sorted(x[0] for x in fr)
        worst = ms[-max(1, len(ms) // 100):]
        print(f'{m:6d} | {len(ms) / (sum(ms) / 1000):4.1f} | {1000 / (sum(worst) / len(worst)):6.1f} | '
              f'{sum(x > 33.3 for x in ms):6d} | {sum(x[1] for x in fr) / len(fr):5.1f}')


if __name__ == '__main__':
    main()
