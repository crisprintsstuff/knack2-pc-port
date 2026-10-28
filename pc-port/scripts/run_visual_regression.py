"""Isolated approved/current visual A/B with approved functional settings.

The selected EXE and DLL are copied together so screenshots and temporary user
files never touch either preserved release. run_probe makes separate copies of
the city save and trusted pipeline cache for each finite run. The preserved
approved DLL has periodic GameOnly capture; a newly built current DLL must
register the opt-in F10 sequence hook before --current can run.
"""

import argparse
from datetime import datetime
import hashlib
import json
import os
from pathlib import Path
import shutil

import run_probe


ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'out'
RELEASES = {
    'approved': {
        'directory': '20261011-upload-fix',
        'iss-probe.exe': 'e7525764420f2055a842ba3fef6d162221f482f68dd64363ae214802ab233b14',
        'iss-gpu.dll': '9936cb5fe1aab4c8ea4334f5e90666199f75478035e261670f10ecec9d69488a',
    },
    'current': {
        # The 15:15 diagnostic DLL predates F10 capture. Root registers the new
        # isolated build here after compiling the opt-in hook.
        'directory': '20261013-visual-sequence',
    },
}

EXPERIMENTAL_OFF = (
    'ISS_CP_SPLIT', 'ISS_ASYNC_COMPUTE', 'ISS_COMPUTE_FAIRNESS',
    'ISS_NATIVE_REDUCTION', 'ISS_NATIVE_CITY_FETCH', 'ISS_NATIVE_DRAW_STATE',
    'ISS_NATIVE_DRAW_STATE_VERIFY', 'ISS_NATIVE_GEOMETRY',
    'ISS_NATIVE_GEOMETRY_STATS', 'ISS_NATIVE_INDEXED_BATCH',
    'ISS_NATIVE_INSTANCED_BATCH', 'ISS_NATIVE_INSTANCED_FEATURE',
    'ISS_NATIVE_READBACK_PAGES', 'ISS_NATIVE_CPU_DRAWS',
    'ISS_DRAW_PIPE', 'ISS_PROFILE_CITY_DRAW_GPU',
    'ISS_FEEDBACK_IDENTITY', 'ISS_FEEDBACK_JOIN', 'ISS_FEEDBACK_ARENA',
    'ISS_FEEDBACK_TRACE', 'ISS_FEEDBACK_PAGE_READBACK',
    'ISS_FEEDBACK_EARLY_SUBMIT', 'ISS_NATIVE_FEEDBACK_PROBE',
    'ISS_PROBE_HOST_IMPORT', 'ISS_SECONDARY_BATCH_PROBE',
    'ISS_NATIVE_MEMORY_REPORT', 'ISS_COMMAND_TRACE',
    'ISS_TIMELINE', 'ISS_DIAGNOSTIC_SESSION', 'ISS_DEEP_PROFILE',
    'ISS_GPU_PROFILE', 'ISS_PIPELINE_EVENTS', 'ISS_CAPTURE_OVERLAY',
)


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def checked_pair(mode):
    spec = RELEASES[mode]
    release = OUT / 'releases' / spec['directory']
    if mode == 'current':
        manifest_path = release / 'visual-capture-build.json'
        if not manifest_path.is_file():
            raise ValueError(f'F10 visual build not registered: {manifest_path}')
        manifest = json.loads(manifest_path.read_text(encoding='utf-8'))
        if manifest.get('visual_sequence_hook') is not True or \
                manifest.get('base_release') != '20261013-diagnostico-frame':
            raise ValueError(f'visual capture manifest does not identify the expected build: {manifest_path}')
        hashes = manifest.get('binaries', {})
    else:
        hashes = {name: spec[name] for name in ('iss-probe.exe', 'iss-gpu.dll')}
    pair = {name: release / name for name in ('iss-probe.exe', 'iss-gpu.dll')}
    for name, path in pair.items():
        if not path.is_file() or not isinstance(hashes.get(name), str) or \
                len(hashes[name]) != 64 or sha256(path) != hashes[name].lower():
            raise ValueError(f'{mode} {name} missing or SHA-256 differs: {path}')
    return pair, hashes


def controlled_environment(staged_exe, mode):
    # An inherited experiment flag must not silently alter only one side.
    env = {key: value for key, value in os.environ.items()
           if not key.startswith(('ISS_', 'SHADPS4_', 'BB_'))}
    run_probe.apply_approved_environment(env, OUT)
    env.update({key: '0' for key in EXPERIMENTAL_OFF})
    env.update({
        'ISS_PROBE_EXE': str(staged_exe),
        'ISS_SAVE_SEED': str(OUT / 'user' / 'city-savedata'),
        'ISS_MINIMAL_LOGS': '0',  # Keep frames.csv, io.tsv, and visible logs.
        # The preserved approved DLL predates F10. Keep its existing periodic
        # GameOnly capture as the visual control; current uses F10 sequences.
        'ISS_CAPTURE_FRAMES': '0' if mode == 'current' else '1',
        'ISS_VISUAL_SEQUENCE': '1' if mode == 'current' else '0',
        'ISS_GPU_PROFILE': '0',
        'ISS_PIPELINE_EVENTS': '0',
        'ISS_TIMELINE': '0',
        'ISS_DEEP_PROFILE': '0',
    })
    if not Path(env['ISS_SAVE_SEED']).is_dir():
        raise ValueError(f'city save seed missing: {env["ISS_SAVE_SEED"]}')
    return env


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    selected = parser.add_mutually_exclusive_group(required=True)
    selected.add_argument('--approved', action='store_true', help='use the preserved 11 October EXE/DLL')
    selected.add_argument('--current', action='store_true', help='use the 13 October diagnostic EXE/DLL')
    parser.add_argument('--seconds', type=int, required=True, help='finite run duration')
    parser.add_argument('--dry-run', action='store_true', help='verify inputs and print settings without launching')
    args = parser.parse_args()
    if args.seconds <= 0:
        parser.error('--seconds must be positive')

    mode = 'approved' if args.approved else 'current'
    try:
        pair, hashes = checked_pair(mode)
    except (OSError, ValueError, KeyError) as error:
        parser.error(str(error))
    planned_exe = OUT / 'visual-regression' / '<timestamp>' / mode / 'iss-probe.exe'
    env = controlled_environment(planned_exe, mode)
    if args.dry_run:
        print(json.dumps({'mode': mode, 'seconds': args.seconds,
                          'source_pair': {k: str(v) for k, v in pair.items()},
                          'source_sha256': hashes,
                          'save_seed': env['ISS_SAVE_SEED'],
                          'cache_seed': str(OUT / 'pipeline-library-runtime-trusted'),
                          'flags': {k: v for k, v in env.items()
                                    if k.startswith(('ISS_', 'SHADPS4_'))}},
                         indent=2))
        return 0

    stamp = datetime.now().strftime('%Y%m%d-%H%M%S-%f')
    stage = OUT / 'visual-regression' / stamp / mode
    stage.mkdir(parents=True, exist_ok=False)
    for name, source in pair.items():
        destination = stage / name
        shutil.copy2(source, destination)
        if sha256(destination) != hashes[name].lower():
            raise ValueError(f'staged {name} failed SHA-256 verification')
    env = controlled_environment(stage / 'iss-probe.exe', mode)
    selection = {'mode': mode, 'seconds': args.seconds, 'staged_pair': str(stage),
                 'source_pair': {k: str(v) for k, v in pair.items()},
                 'sha256': hashes,
                 'save_seed': env['ISS_SAVE_SEED'],
                 'cache_seed': str(OUT / 'pipeline-library-runtime-trusted'),
                 'capture_mode': 'F10 five-frame sequence' if mode == 'current'
                                 else 'periodic GameOnly (approved DLL has no F10 hook)',
                 'game_only_screenshots_under': str(stage / 'user')}
    (stage / 'visual-selection.json').write_text(json.dumps(selection, indent=2) + '\n',
                                                 encoding='utf-8')
    # run_probe reads os.environ, then copies the seed/cache into its new run directory.
    os.environ.clear()
    os.environ.update(env)
    result = run_probe.run(entry=True, linked=True, gpu=True, timeout=args.seconds)
    record = json.loads((OUT / 'latest-run.json').read_text(encoding='utf-8'))
    run_dir = Path(record['run_dir'])
    (run_dir / 'visual-selection.json').write_text(json.dumps(selection, indent=2) + '\n',
                                                    encoding='utf-8')
    print(f'Visual run: {run_dir}\nGame-only screenshots: {stage / "user"}')
    return 0 if record.get('exit_code') == 0 and not record.get('corruption_dialog') else result or 1


if __name__ == '__main__':
    raise SystemExit(main())
