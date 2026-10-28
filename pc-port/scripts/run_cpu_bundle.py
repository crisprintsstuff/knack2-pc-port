"""A/B the CPU resource bundle against the same DLL's ordinary draw path."""
import argparse
import os
import json
import sys
import subprocess
import threading
import time
from pathlib import Path
import run_probe
from run_cpu_native import verify_batch_shaders
ROOT=Path(__file__).resolve().parents[1]

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--control',action='store_true')
    parser.add_argument('--seconds',type=int,default=140)
    parser.add_argument('--captures',action='store_true')
    parser.add_argument('--profile',action='store_true')
    args=parser.parse_args()
    if args.seconds<=0:parser.error('Use a finite duration to isolate save files')
    out=ROOT/'out';verify_batch_shaders(out)
    run_probe.apply_approved_environment(os.environ,out)
    os.environ.update(ISS_PROBE_EXE=str(out/'releases/20261013-native-cpu-bundle/iss-probe.exe'),
        ISS_SAVE_SEED=str(out/'user/city-savedata'),ISS_MINIMAL_LOGS='0',
        ISS_NATIVE_GEOMETRY='0',ISS_NATIVE_DRAW_STATE='0',ISS_NATIVE_DRAW_STATE_VERIFY='0',
        ISS_NATIVE_INDEXED_BATCH='0',ISS_NATIVE_INSTANCED_BATCH='0',ISS_NATIVE_INSTANCED_FEATURE='0',
        ISS_NATIVE_READBACK_PAGES='0',ISS_NATIVE_CITY_FETCH='0',ISS_DRAW_PIPE='0',
        ISS_PROFILE_CITY_DRAW_GPU='0',ISS_GPU_PROFILE='0',ISS_TIMELINE='0',
        ISS_DEEP_PROFILE='0',ISS_FEEDBACK_JOIN='0',ISS_READBACK_DIAG='0',
        ISS_NATIVE_CPU_DRAWS='0' if args.control else '1',ISS_CPU_BUNDLE_SPV_VERIFIED='1',
        ISS_CAPTURE_FRAMES='1' if args.captures else '0')
    stop=threading.Event();monitor=None;started=time.time()
    if args.profile:
        from run_cpu_command_profile import tail_frames
        def sample():
            while not stop.wait(2):
                try:
                    record=json.loads((out/'latest-run.json').read_text());folder=Path(record['run_dir'])
                    if Path(record['executable_path'])!=Path(os.environ['ISS_PROBE_EXE']) or 'pid' not in record:continue
                    if (folder/'run.json').stat().st_mtime<started:continue
                    rows=tail_frames(folder/'frames.csv')
                    if len(rows)<180 or rows[-1]['frame']<1200 or sum(r['pipeline_stall_ms'] for r in rows)>1:continue
                    with (folder/'cpu-profile-console.log').open('w',encoding='utf-8') as log:
                        subprocess.run([sys.executable,str(ROOT/'scripts/profile_threads.py'),'--pid',str(record['pid']),
                            '--thread-name','GpuCommandProcessor','--threads','1','--hz','250','--seconds','20',
                            '--map',str(out/'releases/20261013-native-cpu-bundle/iss-gpu.map'),
                            '--out',str(folder/'cpu-command-profile.json')],stdout=log,stderr=subprocess.STDOUT,timeout=35)
                    return
                except (OSError,ValueError,KeyError):continue
                except subprocess.TimeoutExpired:return
        monitor=threading.Thread(target=sample,daemon=True);monitor.start()
    try:status=run_probe.run(entry=True,linked=True,gpu=True,timeout=args.seconds)
    finally:
        stop.set()
        if monitor:monitor.join(timeout=40)
    record=json.loads((out/'latest-run.json').read_text())
    return 0 if record.get('exit_code')==0 and not record.get('corruption_dialog') else status or 1

if __name__=='__main__':raise SystemExit(main())
