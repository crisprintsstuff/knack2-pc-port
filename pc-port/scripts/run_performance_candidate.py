"""Launch the measured conservative ThinLTO candidate; stable 60 FPS is not achieved."""
import argparse
import hashlib
import json
import os
from pathlib import Path

import run_probe
from run_visual_regression import EXPERIMENTAL_OFF

OUT = Path(__file__).resolve().parents[1] / 'out'
RELEASE = OUT / 'releases/20261013-performance-session'
EXPECTED = {
    'iss-probe.exe': 'c974c7dcb3b8b5b33c1f5a15a0ca1ee432484492d7ba69b948ff2c6a8edd09fd',
    'iss-gpu.dll': '77ba0c4555492832fa928ffb2a5f7afd53ccf9f3f66c79758533d0c4f80d2beb',
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true')
    parser.add_argument('--seconds', type=int, default=0,
                        help='0 plays until closed; a positive duration tests with a save copy')
    args = parser.parse_args()
    if args.seconds < 0:
        parser.error('--seconds cannot be negative')
    for name, digest in EXPECTED.items():
        if hashlib.sha256((RELEASE / name).read_bytes()).hexdigest() != digest:
            raise ValueError(f'Tested runtime identity changed: {name}')
    display = {key: os.environ[key] for key in ('ISS_FULLSCREEN', 'ISS_WINDOW_ICON')
               if key in os.environ}
    env = {key: value for key, value in os.environ.items()
           if not key.startswith(('ISS_', 'SHADPS4_', 'BB_'))}
    run_probe.apply_approved_environment(env, OUT)
    env.update({key: '0' for key in EXPERIMENTAL_OFF})
    env.update(display)
    env.update(ISS_PROBE_EXE=str(RELEASE / 'iss-probe.exe'),
               ISS_SAVE_SEED=str(OUT / 'user/city-savedata'),
               ISS_CLAMP_CACHE_FIRST='1', ISS_MINIMAL_LOGS='1')
    if not (Path(env['ISS_SAVE_SEED']) / '1/CUSA00004/infamousssAuto0/dis.dat').is_file():
        raise FileNotFoundError('City save is missing')
    if args.check:
        print(json.dumps({'release': str(RELEASE), 'sha256': EXPECTED,
                          'save_seed': env['ISS_SAVE_SEED'],
                          'stable_60_fps_verified': False,
                          'native_cpu_bundle': False}, indent=2))
        return 0
    os.environ.clear()
    os.environ.update(env)
    print('Experimental ThinLTO candidate. Stable 60 FPS has not been achieved.', flush=True)
    return run_probe.run(entry=True, linked=True, gpu=True, timeout=args.seconds)


if __name__ == '__main__':
    raise SystemExit(main())
