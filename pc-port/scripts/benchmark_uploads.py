"""Compare serial and worker uploads using identical isolated save snapshots."""
import csv
from datetime import datetime
import hashlib
import json
import os
from pathlib import Path
import shutil
import statistics
import subprocess
import sys

ROOT=Path(__file__).resolve().parents[1]
destination=ROOT/'out/benchmarks'/datetime.now().strftime('%Y%m%d-%H%M%S')
destination.mkdir(parents=True)
seed=destination/'seed'
shutil.copytree(ROOT/'out/user/savedata',seed)
flag='ISS_DESCRIPTOR_FAST_PATH' if '--descriptors' in sys.argv else 'ISS_PARALLEL_RENDER' if '--parallel' in sys.argv else 'ISS_RENDER_REUSE' if '--render' in sys.argv else 'ISS_REAL_OCCLUSION' if '--occlusion' in sys.argv else 'ISS_BULK_COPY' if '--bulk' in sys.argv else 'ISS_UPLOAD_WORKER'
if '--readback' in sys.argv:flag='ISS_READBACK_WINDOW_KB'
if '--reduction' in sys.argv:flag='ISS_PRECISE_REDUCTION_READBACK'
if '--native' in sys.argv:flag='ISS_NATIVE_REDUCTION'
if '--compute-fairness' in sys.argv:flag='ISS_COMPUTE_FAIRNESS'
if '--feedback-page' in sys.argv:flag='ISS_FEEDBACK_PAGE_READBACK'
if '--feedback-submit' in sys.argv:flag='ISS_FEEDBACK_EARLY_SUBMIT'
if '--async-compute' in sys.argv:flag='ISS_ASYNC_COMPUTE'
if '--mapping-cache' in sys.argv:flag='ISS_MAPPING_CACHE'
if '--fault-window' in sys.argv:flag='ISS_FAULT_WINDOW_KB'
report={'option':flag,'seed_hashes':{str(p.relative_to(seed)):hashlib.sha256(p.read_bytes()).hexdigest() for p in seed.rglob('*') if p.is_file()},'runs':[]}
for mode in (('512','64') if '--readback' in sys.argv else ('0','64') if '--fault-window' in sys.argv else ('0','1')):
    env=os.environ.copy();env.update(ISS_UPLOAD_WORKER='0',ISS_BULK_COPY='1',ISS_SAVE_SEED=str(seed),ISS_CAPTURE_FRAMES='0',ISS_GPU_PROFILE='0');env[flag]=mode
    if any(a in sys.argv for a in ('--render','--occlusion','--parallel','--descriptors','--readback','--reduction','--native','--compute-fairness','--feedback-page','--feedback-submit','--async-compute','--mapping-cache','--fault-window')):env['ISS_GPU_PROFILE']='1'
    with (destination/f'{flag}-{mode}.log').open('w',encoding='utf-8') as log:
        subprocess.run([sys.executable,str(ROOT/'scripts/run_probe.py'),'--linked','--entry','--gpu','--timeout','80'],env=env,stdout=log,stderr=subprocess.STDOUT,check=False)
    run=json.loads((ROOT/'out/latest-run.json').read_text(encoding='utf-8'));rows=[]
    if run.get('exit_code')!=0 or not run.get('timeout') or run.get('forced_stop') or run.get('events') or run.get('io_summary',{}).get('failed_transfers'):
        raise RuntimeError(f'Incomplete or failed run; not a valid comparison: {run["run_dir"]}')
    with (Path(run['run_dir'])/'frames.csv').open() as stream:
        for row in csv.DictReader(stream):
            try:rows.append({k:float(v) for k,v in row.items()})
            except (ValueError,TypeError):continue
    if len(rows)<600 or sum(r['present_ms'] for r in rows)<30000:
        raise RuntimeError(f'Insufficient measured gameplay window: {run["run_dir"]}')
    report['runs'].append({'value':mode,'run_dir':run['run_dir'],'exit_code':run.get('exit_code'),'events':run.get('events'),'io_errors':run.get('io_summary',{}).get('failed_transfers'),'rows':rows})
    (destination/'raw.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
    print(f'Finished {flag}={mode}; {len(rows)} frame records',flush=True)
common=min(len(r['rows']) for r in report['runs'])
summaries=[]
for run in report['runs']:
    all_rows=run.pop('rows');rows=all_rows[:common];times=sorted(r['present_ms'] for r in rows)
    if not rows:raise RuntimeError('No comparable frames')
    run.update(frames=common,median_ms=statistics.median(times),p95_ms=times[int(len(times)*.95)],max_ms=max(times),over_50ms=sum(t>50 for t in times),
               totals={k:sum(r[k] for r in rows) for k in ('shader_compile_ms','pipeline_stall_ms','pipeline_skips','buffer_upload_bytes','upload_worker_bytes','upload_worker_work_ms','upload_worker_wait_ms','command_processor_ms')})
    summaries.append(run)
    tail=[];duration=0
    for row in reversed(all_rows):
        tail.append(row);duration+=row['present_ms']
        if duration>=30000:break
    ordered=sorted(r['present_ms'] for r in tail)
    run['tail_30s']={'fps':len(tail)*1000/duration,'p95_ms':ordered[int(len(ordered)*.95)],'mean':{k:statistics.mean(r[k] for r in tail) for k in ('command_processor_ms','gpu_completed_work_ms','readback_stall_ms','shader_compile_ms','pipeline_stall_ms','pipeline_skips','draws','dispatches')}}
report['comparison_note']='Same save seed and first N guest flips; not a simulation-FPS claim. File/driver/OS caches may warm between runs. No skipped shader work is permitted.'
(destination/'comparison.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
print(json.dumps(report,indent=2))
