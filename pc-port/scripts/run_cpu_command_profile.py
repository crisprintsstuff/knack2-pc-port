"""Bounded city run: sample only the command processor after a quiet compile window."""
import csv
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import threading
import time
import run_probe
ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'out'
RELEASE=OUT/'releases/20261013-cpu-command-profile'

def tail_frames(path):
    try:
        with path.open('rb') as source:
            header=source.readline();base=source.tell();source.seek(0,2);end=source.tell()
            source.seek(max(base,end-131072))
            if source.tell()>base:source.readline()
            text=header+source.read()
        rows=[]
        for row in csv.DictReader(io.StringIO(text.decode('utf-8'))):
            try:rows.append({k:float(v) for k,v in row.items()})
            except (TypeError,ValueError):pass
        return rows[-180:]
    except OSError:return []

def main():
    run_probe.apply_approved_environment(os.environ,OUT)
    os.environ.update(ISS_PROBE_EXE=str(RELEASE/'iss-probe.exe'),
        ISS_SAVE_SEED=str(OUT/'user/city-savedata'),ISS_MINIMAL_LOGS='0',
        ISS_NATIVE_GEOMETRY='0',ISS_NATIVE_DRAW_STATE='0',ISS_NATIVE_DRAW_STATE_VERIFY='0',
        ISS_NATIVE_INDEXED_BATCH='0',ISS_NATIVE_INSTANCED_BATCH='0',ISS_NATIVE_INSTANCED_FEATURE='0',
        ISS_NATIVE_READBACK_PAGES='0',ISS_NATIVE_CITY_FETCH='0',ISS_DRAW_PIPE='0',
        ISS_PROFILE_CITY_DRAW_GPU='0',ISS_GPU_PROFILE='0',ISS_TIMELINE='0',
        ISS_DEEP_PROFILE='0',ISS_FEEDBACK_JOIN='0',ISS_READBACK_DIAG='0')
    started=time.time();stop=threading.Event();result={}
    def capture():
        while not stop.wait(2):
            try:
                record=json.loads((OUT/'latest-run.json').read_text())
                if Path(record['executable_path'])!=RELEASE/'iss-probe.exe' or not record.get('pid'):continue
                folder=Path(record['run_dir'])
                if (folder/'run.json').stat().st_mtime<started:continue
                rows=tail_frames(folder/'frames.csv')
                if len(rows)<180 or rows[-1]['frame']<1200:continue
                if sum(r['pipeline_stall_ms'] for r in rows)>1:continue
                if sum(r['draws'] for r in rows)/len(rows)<7500:continue
                result.update(run_dir=str(folder),trigger_frame=rows[-1]['frame'],
                    preceding_draws=sum(r['draws'] for r in rows)/len(rows),
                    preceding_compile_ms=sum(r['pipeline_stall_ms'] for r in rows))
                with (folder/'cpu-profile-console.log').open('w',encoding='utf-8') as log:
                    run=subprocess.run([sys.executable,str(ROOT/'scripts/profile_threads.py'),
                        '--pid',str(record['pid']),'--thread-name','GpuCommandProcessor',
                        '--threads','1','--hz','250','--seconds','20',
                        '--map',str(RELEASE/'iss-gpu.map'),'--out',str(folder/'cpu-command-profile.json')],
                        stdout=log,stderr=subprocess.STDOUT,timeout=35)
                result['profile_exit']=run.returncode
                (folder/'cpu-profile-trigger.json').write_text(json.dumps(result,indent=2))
                return
            except (OSError,ValueError,KeyError):continue
            except subprocess.TimeoutExpired:
                result['error']='profiler timeout';return
    monitor=threading.Thread(target=capture,daemon=True);monitor.start()
    try:status=run_probe.run(entry=True,linked=True,gpu=True,timeout=140)
    finally:stop.set();monitor.join(timeout=40)
    print('CPU_PROFILE_RESULT',json.dumps(result))
    return 0 if result.get('profile_exit')==0 else status or 1

if __name__=='__main__':raise SystemExit(main())
