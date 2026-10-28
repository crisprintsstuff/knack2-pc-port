"""Isolated cold/warm shader experiment with a repeatable power sequence."""
import argparse
from pathlib import Path
import shutil
import subprocess
import sys
import time
import json
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(); p.add_argument('label'); p.add_argument('--dll',type=Path,required=True)
p.add_argument('--set',action='append',default=[]); p.add_argument('--seed',type=Path)
p.add_argument('--seconds',type=int,default=90); p.add_argument('--no-replay',action='store_true')
a=p.parse_args()
tasks=subprocess.check_output(['tasklist','/FI','IMAGENAME eq iss-probe.exe','/FO','CSV'],text=True)
if 'iss-probe.exe' in tasks.lower(): raise SystemExit('iss-probe already running')
runtime=ROOT/'out/experimental'/a.label
runtime.mkdir(exist_ok=False)
shutil.copy2(ROOT/'out/experimental/20261015-shaders-replay/iss-probe.exe',runtime/'iss-probe.exe')
shutil.copy2(a.dll,runtime/'iss-gpu.dll')
lineage=ROOT/'out'/('codex-cache-'+a.label)
if a.seed: shutil.copytree(a.seed,lineage/'current')
else:
    seed=lineage/'current/CUSA00004'; seed.mkdir(parents=True)
    shutil.copy2(ROOT/'out/pipeline-cache-claude-dev/current/CUSA00004/profile.bin',seed)
flags='''ISS_CLAMP_CACHE_FIRST=1 ISS_STAGE_MEMO=1 ISS_FAST_DRAW=1 ISS_FAST_REBIND=1
ISS_FAST_LEAN=1 ISS_FAST_SWITCH=1 ISS_SUBMIT_EVERY=1000 BB_VBLANK_HZ=240
ISS_COMPUTE_FAIRNESS=1 ISS_CP_CPU=14 ISS_STAGE_MEMO_LOOSE=1 ISS_STAGE_MEMO_TEXEL=1
ISS_LAZY_PIPELINES=1 ISS_SPEC_TRANSLATE=1'''.split()
if not a.no_replay: flags.append('ISS_SHADER_POWER_REPLAY=1')
flags+=a.set
cmd=[sys.executable,str(ROOT/'scripts/run_controlled_benchmark.py'), '--exe',str(runtime/'iss-probe.exe'),
     '--label',a.label,'--seconds',str(a.seconds),'--cache-lineage',str(lineage)]
for flag in flags: cmd+=['--set',flag]
with (runtime/'benchmark.log').open('w',encoding='utf-8') as log:
    (runtime/'launch-clock.json').write_text(json.dumps(dict(clock='Windows QPC',
        benchmark_start_ns=time.perf_counter_ns(),wall_time=time.time())),encoding='utf-8')
    r=subprocess.run(cmd,cwd=ROOT,stdout=log,stderr=subprocess.STDOUT)
lines=(runtime/'benchmark.log').read_text(encoding='utf-8').splitlines()
print('\n'.join(line for line in lines if line.startswith(('{"label"','{"valid_window"','CACHE_LINEAGE'))))
raise SystemExit(r.returncode)
