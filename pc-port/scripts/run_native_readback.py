"""A/B the page provenance implementation with identical binaries/cache/save seed."""
import argparse
import json
import os
from pathlib import Path
import run_probe

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--control',action='store_true')
    parser.add_argument('--seconds',type=int,default=140)
    args=parser.parse_args()
    if args.seconds<=0:parser.error('a finite duration is required for isolated saves')
    out=Path(__file__).resolve().parents[1]/'out'
    run_probe.apply_approved_environment(os.environ,out)
    os.environ.update(
        ISS_PROBE_EXE=str(out/'releases/20261013-native-readback-pages/iss-probe.exe'),
        ISS_SAVE_SEED=str(out/'user/city-savedata'),
        ISS_NATIVE_GEOMETRY='0',ISS_NATIVE_DRAW_STATE='0',ISS_NATIVE_DRAW_STATE_VERIFY='0',
        ISS_NATIVE_INDEXED_BATCH='0',ISS_NATIVE_INSTANCED_BATCH='0',
        ISS_NATIVE_CITY_FETCH='0',ISS_PROFILE_CITY_DRAW_GPU='0',ISS_DRAW_PIPE='0',
        ISS_MINIMAL_LOGS='0',ISS_FEEDBACK_IDENTITY='0',ISS_NATIVE_READBACK_PAGES='0' if args.control else '1')
    result=run_probe.run(entry=True,linked=True,gpu=True,timeout=args.seconds)
    record=json.loads((out/'latest-run.json').read_text())
    return 0 if record.get('exit_code')==0 and not record.get('corruption_dialog') else result or 1

if __name__=='__main__':raise SystemExit(main())
