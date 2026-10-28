"""Run the isolated CPU port with the approved shader pack and trusted seed only."""
import argparse
import json
import hashlib
import os
from pathlib import Path
import run_probe

ROOT = Path(__file__).resolve().parents[1]

def verify_batch_shaders(out):
    expected = {
        '0x000000000beb7156_0.spv': '6f3f0e8fcb0f7d81d6fa06a5390b4441e4b8875d2982857980c8863431c1f95f',
        '0x000000006353e48b_0.spv': '5598f6eb089875eddcb40f6e250135914718e627b193af27dd50f7db7910c795',
    }
    pack = out / 'pipeline-library/CUSA00004-packed'
    assets = {a['filename']: a for a in json.loads((pack/'manifest.json').read_text())['assets']}
    with (pack/'recipes.bin').open('rb') as source:
        for name, digest in expected.items():
            item = assets[name]
            source.seek(item['offset'])
            if item['sha256'] != digest or hashlib.sha256(source.read(item['size'])).hexdigest() != digest:
                raise ValueError(f'native batch requires the audited shader: {name}')
            override = out / 'pipeline-library-runtime-trusted/CUSA00004' / name
            if override.exists() and hashlib.sha256(override.read_bytes()).hexdigest() != digest:
                raise ValueError(f'trusted shader overrides the audited module: {name}')
    return expected

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--seconds', type=int, default=120)
    parser.add_argument('--control', action='store_true', help='Use original geometry emitter')
    parser.add_argument('--geometry', action='store_true', help='Explicitly enable the experimental CPU geometry port')
    parser.add_argument('--draw-state', action='store_true', help='Explicitly enable experimental dynamic state reuse')
    parser.add_argument('--approved', action='store_true', help='Use the unchanged approved EXE/DLL for visual control')
    parser.add_argument('--batch', action='store_true', help='Test the native indexed batch release (off with --control)')
    parser.add_argument('--captures', action='store_true', help='Capture game frames for visual inspection')
    parser.add_argument('--stats', action='store_true', help='Record hook execution counts')
    args = parser.parse_args()
    if args.seconds <= 0:
        parser.error('--seconds must be positive so saves remain isolated')
    out = ROOT / 'out'
    if args.batch:
        verify_batch_shaders(out)
    run_probe.apply_approved_environment(os.environ, out)
    os.environ.pop('ISS_PIPELINE_CACHE_SEED', None)
    os.environ.pop('ISS_PIPELINE_CACHE_ROOT', None)
    os.environ.update(
        ISS_PROBE_EXE=str(out / ('releases/20261013-native-indexed-batch/iss-probe.exe'
                                if args.batch else 'releases/20261013-native-cpu/iss-probe.exe')),
        ISS_SAVE_SEED=str(out / 'user/city-savedata'),
        ISS_NATIVE_GEOMETRY='1' if args.geometry and not args.control and not args.approved else '0',
        ISS_NATIVE_GEOMETRY_STATS='1' if args.stats else '0',
        ISS_NATIVE_DRAW_STATE='1' if args.draw_state and not args.control and not args.approved else '0', ISS_NATIVE_DRAW_STATE_VERIFY='0',
        ISS_NATIVE_INDEXED_BATCH='1' if args.batch and not args.control and not args.approved else '0',
        ISS_CAPTURE_FRAMES='1' if args.captures else '0',
        ISS_NATIVE_CITY_FETCH='0', ISS_PROFILE_CITY_DRAW_GPU='0',
        ISS_DRAW_PIPE='0', ISS_MINIMAL_LOGS='0')
    status = run_probe.run(entry=True, linked=True, gpu=True, timeout=args.seconds,
                           approved=args.approved)
    record = json.loads((out / 'latest-run.json').read_text(encoding='utf-8'))
    if record.get('exit_code') == 0 and not record.get('corruption_dialog'):
        return 0
    return status or 1

if __name__ == '__main__':
    raise SystemExit(main())
