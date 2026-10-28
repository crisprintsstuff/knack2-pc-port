# SPDX-License-Identifier: GPL-2.0-or-later
"""Run the isolated probe with a deadline, integrity gate and machine-readable evidence."""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import subprocess
import os
import shutil
from process_metrics import start_metrics
from datetime import datetime

ROOT = Path(__file__).resolve().parents[1]

APPROVED_FLAGS = {
    'ISS_RING_BARRIERS': '1', 'ISS_REAL_OCCLUSION': '1', 'ISS_CP_SPLIT': '0',
    'ISS_PREWARM_LARGE_UPLOAD': '0', 'ISS_MINIMAL_LOGS': '1',
    'ISS_GPU_PROFILE': '0', 'ISS_RENDER_REUSE': '1', 'ISS_PARALLEL_RENDER': '1',
    'ISS_DESCRIPTOR_FAST_PATH': '1', 'ISS_FAULT_WINDOW_KB': '64',
    'ISS_UPLOAD_WORKER': '1',
    'ISS_TUNING': 'async_written_uploads=1,scout=1,scout_fast=1,bind_prefetch=1',
    'ISS_BARRIER_POOL': '1', 'ISS_MAPPING_CACHE': '1',
    'SHADPS4_FAST_STAGE_LOOKUP': '1', 'ISS_TLS_PATCH': '1',
    'ISS_THREAD_PRIORITY': '1', 'ISS_MAPPING_INDEX': '1',
    'ISS_COMPUTE_PHASES': '0', 'ISS_COMMAND_TRACE': '0',
    'ISS_CAPTURE_FRAMES': '0', 'ISS_DEEP_PROFILE': '0',
    'ISS_FEEDBACK_TRACE': '0', 'ISS_FEEDBACK_IDENTITY': '0',
    'ISS_G4_FEEDBACK_PROBE': '0', 'ISS_READBACK_DIAG': '0',
    'ISS_TRACE_READBACK': '0', 'ISS_GPU_HAZARD_DIAG': '0',
    'ISS_NATIVE_FEEDBACK_PROBE': '0', 'ISS_PROBE_HOST_IMPORT': '0',
    'ISS_ASYNC_COMPUTE': '0', 'ISS_COMPUTE_FAIRNESS': '0',
    'ISS_FEEDBACK_EARLY_SUBMIT': '0', 'ISS_FEEDBACK_PAGE_READBACK': '0',
    'ISS_NATIVE_REDUCTION': '0', 'ISS_PIPELINE_EVENTS': '0',
    'ISS_NATIVE_READBACK_PAGES': '0', 'ISS_NATIVE_INSTANCED_BATCH': '0',
    'ISS_FEEDBACK_ARENA': '0', 'ISS_FEEDBACK_JOIN': '0',
    'ISS_SECONDARY_BATCH_PROBE': '0', 'ISS_NATIVE_MEMORY_REPORT': '0',
    'ISS_NATIVE_INSTANCED_FEATURE': '0', 'ISS_TIMELINE': '0',
    'ISS_NATIVE_CPU_DRAWS': '0', 'ISS_CPU_BUNDLE_SPV_VERIFIED': '0',
    'ISS_CLAMP_CACHE_FIRST': '0', 'ISS_CPU_BUNDLE_INDEX_ONCE': '0',
    'ISS_SMALL_READ_MEMO': '0', 'ISS_DESCRIPTOR_DELTAS': '0',
    'ISS_TEXTURE_LOOKUP_MEMO': '0', 'ISS_TEXTURE_LOOKUP_MEMO_STATS': '0',
    'ISS_PIPELINE_STATE_REUSE': '0', 'ISS_PIPELINE_STATE_REUSE_VERIFY': '0',
    'ISS_PIPELINE_STATE_REUSE_STATS': '0',
    'ISS_VISUAL_SEQUENCE': '0',
}

def apply_approved_environment(env, out):
    pack = out / 'pipeline-library/CUSA00004-packed'
    trusted = out / 'pipeline-library-runtime-trusted/CUSA00004/profile.bin'
    if not (pack / 'manifest.json').is_file() or not trusted.is_file():
        raise FileNotFoundError('approved pipeline pack or trusted cache is missing')
    env.update(APPROVED_FLAGS)
    env['ISS_PIPELINE_LIBRARY'] = str(pack)
    env.setdefault('ISS_SAVE_ROOT', str(out / 'user/city-savedata'))
    env.pop('ISS_PIPELINE_CACHE_ROOT', None)
    env.pop('ISS_PIPELINE_CACHE_SEED', None)

def run(entry=False, linked=False, gpu=False, timeout=10, approved=False, isolate_saves=False,
        diagnostic_context=None):
    out = ROOT / 'out'
    analysis = json.loads((out / ('link.json' if linked else 'analysis.json')).read_text(encoding='utf-8'))
    boot = out / ('boot-linked.bin' if linked else 'boot.bin')
    boot_sha = analysis['sha256'] if linked else analysis['probe']['sha256']
    sources = analysis['sources'] if linked else analysis['modules']
    if hashlib.sha256(boot.read_bytes()).hexdigest() != boot_sha:
        raise ValueError('prepared image changed; rerun prepare.py')
    for m in sources:
        if hashlib.sha256(Path(m['path']).read_bytes()).hexdigest() != m['sha256']:
            raise ValueError(f'source identity changed: {m["path"]}')
    mode = '--entry' if entry else '--map-only'
    exe = (out / 'releases/20261011-upload-fix/iss-probe.exe').resolve() if approved else Path(
        os.environ.get('ISS_PROBE_EXE', str(out / 'iss-probe.exe'))).resolve()
    if not exe.is_file():
        raise FileNotFoundError(f'PC port executable missing: {exe}')
    record = {'timestamp': datetime.now().astimezone().isoformat(), 'mode': mode,
              'executable_sha256': hashlib.sha256(exe.read_bytes()).hexdigest(),
              'boot_sha256': boot_sha, 'source_sha256': sources[0]['sha256'], 'linked': linked}
    if gpu:
        record['gpu_sha256']=hashlib.sha256((exe.parent/'iss-gpu.dll').read_bytes()).hexdigest()
    run_dir=out/'runs'/datetime.now().strftime('%Y%m%d-%H%M%S-%f')
    run_dir.mkdir(parents=True)
    if diagnostic_context is not None:
        (run_dir/'diagnostic-context.json').write_text(
            json.dumps(diagnostic_context,indent=2,ensure_ascii=False),encoding='utf-8')
    stdout_path=run_dir/'stdout.log'
    stderr_path=run_dir/'stderr.log'
    io_path=run_dir/'io.tsv'
    stop_path=run_dir/'stop.request'
    io_path.write_text('tick_ms\tthread\top\tfd\toffset\trequested\tcompleted\twinerror\terrno\tpath\n',encoding='utf-8')
    record.update(gpu=gpu, executable_path=str(exe), timeout_seconds=timeout, run_dir=str(run_dir),
                  stdout_path=str(stdout_path),stderr_path=str(stderr_path),io_path=str(io_path))
    # The city autosaves are the default test route. Keep a writable copy
    # separate from SaveBackup and honor an explicit seed/root for other scenes.
    city_saves = out / 'user/city-savedata'
    save_source=Path(os.environ.get('ISS_SAVE_SEED',
                                    os.environ.get('ISS_SAVE_ROOT', str(city_saves))))
    if gpu and save_source.exists():
        shutil.copytree(save_source,run_dir/'savedata-before')
    record['save_seed']=str(save_source)
    (out/'latest-run.json').write_text(json.dumps(record,indent=2),encoding='utf-8')
    try:
        env=os.environ.copy()
        if approved:
            apply_approved_environment(env, out)
        minimal_logs = gpu and env.get('ISS_MINIMAL_LOGS') == '1'
        record['minimal_logs'] = minimal_logs
        env['ISS_GAME_DIR']=str(Path(sources[0]['path']).parent)
        env['ISS_GPU']='1' if gpu else '0'
        diagnostic_paths = {
            'ISS_IO_LOG': io_path,
            'SHADPS4_FRAME_CSV': run_dir/'frames.csv',
            'ISS_DEEP_LOG': run_dir/'cpu-phases.csv',
            'ISS_ACCESS_TIMES_LOG': run_dir/'memory-access.csv',
            'ISS_FEEDBACK_LOG': run_dir/'feedback-bindings.csv',
            'ISS_DEPENDENCY_LOG': run_dir/'readback-dependencies.csv',
            'ISS_COMMAND_TRACE_LOG': run_dir/'commands.jsonl',
            'ISS_SECONDARY_BATCH_PROBE_LOG': run_dir/'secondary-batch.jsonl',
            'ISS_SECONDARY_BATCH_PROBE_BYTES': run_dir/'secondary-batch.bin',
            'ISS_FEEDBACK_JOIN_LOG': run_dir/'feedback-join.csv',
            'ISS_READBACK_DIAG_LOG': run_dir/'readback-diag.csv',
            'ISS_TIMELINE_LOG': run_dir/'runtime-timeline.csv',
        }
        for key, path in diagnostic_paths.items():
            if minimal_logs:
                env.pop(key, None)
            else:
                env[key] = str(path)
        env['ISS_STOP_FILE']=str(stop_path)
        if gpu and (timeout>0 or isolate_saves):
            work_saves=run_dir/'savedata-work'
            if (run_dir/'savedata-before').exists():
                shutil.copytree(run_dir/'savedata-before',work_saves)
            else:
                work_saves.mkdir()
            env['ISS_SAVE_ROOT']=str(work_saves)
            record['isolated_save_root']=str(work_saves)
        elif gpu:
            env.setdefault('ISS_SAVE_ROOT', str(save_source))
        env.setdefault('SHADPS4_PARALLEL_PRELOAD','1')
        if gpu:
            env.setdefault('ISS_REAL_OCCLUSION', '1')
            packed_library = out / 'pipeline-library/CUSA00004-packed'
            if (packed_library / 'manifest.json').is_file():
                env.setdefault('ISS_PIPELINE_LIBRARY', str(packed_library))
                if env.get('ISS_PIPELINE_CACHE_SEED') and env.get('ISS_PIPELINE_CACHE_ROOT'):
                    raise ValueError('choose either ISS_PIPELINE_CACHE_SEED or ISS_PIPELINE_CACHE_ROOT')
                if 'ISS_PIPELINE_CACHE_ROOT' not in env:
                    # A runtime recipe written near shutdown crashed the next preload.
                    # Keep the last validated local recipes intact and give each run a
                    # writable copy. New recipes stay with that run for inspection;
                    # the next launch always starts from the validated seed.
                    trusted_cache = out / 'pipeline-library-runtime-trusted'
                    custom_seed = env.get('ISS_PIPELINE_CACHE_SEED')
                    if custom_seed:
                        from build_selected_preload_bundle import verify as verify_selected_seed
                        seed_root = Path(custom_seed).resolve(strict=True)
                        if Path(env['ISS_PIPELINE_LIBRARY']).resolve() != packed_library.resolve():
                            raise ValueError('selected pipeline seed requires the active packed library')
                        verify_selected_seed(seed_root, exe.parent)
                        work_cache = run_dir / 'pipeline-cache'
                        shutil.copytree(seed_root, work_cache)
                        env['ISS_PIPELINE_CACHE_ROOT'] = str(work_cache)
                        record['pipeline_cache_seed'] = str(seed_root)
                        record['pipeline_seed_bundle_sha256'] = hashlib.sha256(
                            (seed_root / 'bundle.json').read_bytes()).hexdigest()
                    elif (trusted_cache / 'CUSA00004/profile.bin').is_file():
                        work_cache = run_dir / 'pipeline-cache'
                        shutil.copytree(trusted_cache, work_cache)
                        env['ISS_PIPELINE_CACHE_ROOT'] = str(work_cache)
                        record['pipeline_cache_seed'] = str(trusted_cache)
                    else:
                        work_cache = run_dir / 'pipeline-cache'
                        work_cache.mkdir()
                        env['ISS_PIPELINE_CACHE_ROOT'] = str(work_cache)
        if env.get('ISS_DESCRIPTOR_FAST_PATH')=='1':
            env['SHADPS4_REUSE_DESCRIPTOR_STORAGE']='1'
            env['SHADPS4_FAST_STAGE_LOOKUP']='1'
        record['shader_settings']={k:env.get(k) for k in ('SHADPS4_PARALLEL_PRELOAD','ISS_PIPELINE_CHECKPOINT','ISS_GPU_PROFILE','ISS_CAPTURE_FRAMES','ISS_UPLOAD_WORKER','ISS_BULK_COPY','ISS_RENDER_REUSE','ISS_REAL_OCCLUSION','ISS_PARALLEL_RENDER','ISS_DESCRIPTOR_FAST_PATH','SHADPS4_REUSE_DESCRIPTOR_STORAGE','SHADPS4_FAST_STAGE_LOOKUP','ISS_READBACK_WINDOW_KB','ISS_TRACE_READBACK','ISS_MINIMAL_LOGS')}
        record['shader_settings']['ISS_PRECISE_REDUCTION_READBACK']=env.get('ISS_PRECISE_REDUCTION_READBACK')
        for key in ('ISS_PIPELINE_LIBRARY','ISS_PIPELINE_CACHE_ROOT','ISS_PIPELINE_CACHE_SEED','ISS_PIPELINE_EVENTS'):
            record['shader_settings'][key]=env.get(key)
        for key in ('ISS_CP_SPLIT','ISS_COMPUTE_FAIRNESS','ISS_ASYNC_COMPUTE','ISS_NATIVE_FEEDBACK_PROBE','ISS_PROBE_HOST_IMPORT','ISS_FEEDBACK_TRACE','ISS_FEEDBACK_PAGE_READBACK','ISS_FEEDBACK_EARLY_SUBMIT','ISS_COMMAND_TRACE','ISS_COMMAND_TRACE_START_SECONDS','ISS_COMMAND_TRACE_FRAMES','ISS_COMPUTE_PHASES','ISS_FAULT_WINDOW_KB','ISS_TUNING','ISS_MAPPING_CACHE'):
            record['shader_settings'][key]=env.get(key)
        record['native_settings']={'ISS_NATIVE_REDUCTION':env.get('ISS_NATIVE_REDUCTION'),
                                   'ISS_NATIVE_CITY_FETCH':env.get('ISS_NATIVE_CITY_FETCH'),
                                   'ISS_NATIVE_DRAW_STATE':env.get('ISS_NATIVE_DRAW_STATE'),
                                   'ISS_NATIVE_DRAW_STATE_VERIFY':env.get('ISS_NATIVE_DRAW_STATE_VERIFY'),
                                   'ISS_NATIVE_GEOMETRY':env.get('ISS_NATIVE_GEOMETRY'),
                                   'ISS_NATIVE_INDEXED_BATCH':env.get('ISS_NATIVE_INDEXED_BATCH'),
                                   'ISS_NATIVE_READBACK_PAGES':env.get('ISS_NATIVE_READBACK_PAGES'),
                                   'ISS_NATIVE_INSTANCED_BATCH':env.get('ISS_NATIVE_INSTANCED_BATCH'),
                                   'ISS_NATIVE_GEOMETRY_STATS':env.get('ISS_NATIVE_GEOMETRY_STATS')}
        record['native_settings']['ISS_NATIVE_CPU_DRAWS']=env.get('ISS_NATIVE_CPU_DRAWS')
        record['native_settings']['ISS_CLAMP_CACHE_FIRST']=env.get('ISS_CLAMP_CACHE_FIRST')
        record['native_settings']['ISS_CPU_BUNDLE_INDEX_ONCE']=env.get('ISS_CPU_BUNDLE_INDEX_ONCE')
        for key in ('ISS_SMALL_READ_MEMO', 'ISS_DESCRIPTOR_DELTAS', 'ISS_TEXTURE_LOOKUP_MEMO',
                    'ISS_TEXTURE_LOOKUP_MEMO_STATS', 'ISS_PIPELINE_STATE_REUSE',
                    'ISS_PIPELINE_STATE_REUSE_VERIFY', 'ISS_PIPELINE_STATE_REUSE_STATS'):
            record['native_settings'][key]=env.get(key)
        record['deep_profile']=env.get('ISS_DEEP_PROFILE')=='1'
        record['diagnostic_session']=env.get('ISS_DIAGNOSTIC_SESSION')=='1'
        if record['diagnostic_session']:
            record['diagnostic_flags']={k:v for k,v in env.items()
                if k.startswith(('ISS_','SHADPS4_'))}
            record['telemetry_limits']={'timeline_max_rows':2000000,'timeline_max_mib':384,
                'writer_flush_ms':100,'cpu_clock':'steady_clock/QPC',
                'gpu_clock':'device timestamps, not CPU-calibrated',
                'observed_guest_frame_is_ownership':False}
        record['passive_probes'] = {k: env.get(k) for k in (
            'ISS_FEEDBACK_IDENTITY', 'ISS_FEEDBACK_JOIN', 'ISS_FEEDBACK_JOIN_START_FRAME',
            'ISS_FEEDBACK_JOIN_FRAMES', 'ISS_SECONDARY_BATCH_PROBE',
            'ISS_SECONDARY_BATCH_PROBE_START_SECONDS', 'ISS_NATIVE_MEMORY_REPORT')}
        with stdout_path.open('wb') as stdout_file,stderr_path.open('wb') as stderr_file:
            child_stdout = subprocess.DEVNULL if minimal_logs else stdout_file
            child_stderr = subprocess.DEVNULL if minimal_logs else stderr_file
            process=subprocess.Popen([str(exe),str(boot),mode],cwd=out,stdout=child_stdout,stderr=child_stderr,env=env,creationflags=subprocess.CREATE_NEW_PROCESS_GROUP)
            record['pid']=process.pid
            metrics_stop,metrics_thread=(None,None) if minimal_logs else start_metrics(process.pid,run_dir/'process.csv')
            thread_metrics_stop,thread_metrics_thread=None,None
            if record['diagnostic_session']:
                from thread_metrics import start_thread_metrics
                thread_metrics_stop,thread_metrics_thread=start_thread_metrics(process.pid,run_dir/'threads.csv')
            (run_dir/'run.json').write_text(json.dumps(record,indent=2),encoding='utf-8')
            (out/'latest-run.json').write_text(json.dumps(record,indent=2),encoding='utf-8')
            if record['diagnostic_session']:
                (out/'latest-diagnostic.json').write_text(json.dumps(record,indent=2),encoding='utf-8')
            try:
                code=process.wait(timeout=timeout if timeout > 0 else None)
            except (subprocess.TimeoutExpired, KeyboardInterrupt) as interruption:
                record['timeout']=isinstance(interruption,subprocess.TimeoutExpired)
                record['interrupted']=isinstance(interruption,KeyboardInterrupt)
                stop_path.touch()
                try:
                    code=process.wait(timeout=15)
                except (subprocess.TimeoutExpired,KeyboardInterrupt):
                    process.kill();process.wait();record['forced_stop']=True;code=None
            finally:
                if process.poll() is None:
                    process.kill();process.wait()
                if metrics_stop is not None:
                    metrics_stop.set();metrics_thread.join(timeout=2)
                if thread_metrics_stop is not None:
                    thread_metrics_stop.set();thread_metrics_thread.join(timeout=2)
        output=stdout_path.read_text(encoding='utf-8',errors='replace')
        errors=stderr_path.read_text(encoding='utf-8',errors='replace')
        record.update(exit_code=code,stdout_tail=output[-12000:],stderr_tail=errors[-12000:])
        record['corruption_dialog']='The disc or Hard Disk Drive is corrupt' in output
        with io_path.open(encoding='utf-8',errors='replace') as log:
            rows=list(csv.DictReader(log,delimiter='\t'))
        transfers=[r for r in rows if r['op'] in ('read','write')]
        record['io_summary']={'operations':len(rows),'transfers':len(transfers),
            'failed_transfers':[r for r in transfers if r['errno']!='0'],
            'short_transfers':[r for r in transfers if r['requested']!=r['completed']]}
        events = [json.loads(line[len('PROBE_EVENT '):]) for line in output.splitlines()
                  if line.startswith('PROBE_EVENT ')]
        record['events'] = events
        with (out / ('linked-imports.csv' if linked else 'imports.csv')).open(encoding='utf-8-sig') as f:
            imports = list(csv.DictReader(f))
        for event in events:
            if 'symbol' in event:
                event['import_evidence'] = next((r for r in imports if (linked or r['owner'] == 'eboot.bin')
                                                and r['symbol'] == event['symbol']), None)
        record['expected_stop'] = (code == 20 and entry and len(events) == 1
                                   and events[0]['status'] in ('unsupported_import', 'unsupported_data_import'))
        record['mapping_passed'] = code == 0 and events == [{'status': 'mapped_only', 'guest_executed': False}]
    except subprocess.TimeoutExpired as e:
        # subprocess.run terminates only this isolated child on timeout, never the game/emulator.
        record.update(exit_code=None, timeout=True,
                      stdout=stdout_path.read_text(encoding='utf-8',errors='replace'),
                      stderr=stderr_path.read_text(encoding='utf-8',errors='replace'))
    destination = out / (('probe-gpu-' if gpu else 'probe-linked-' if linked else 'probe-') + ('entry.json' if entry else 'map.json'))
    destination.write_text(json.dumps(record, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    (run_dir/'run.json').write_text(json.dumps(record,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    (out/'latest-run.json').write_text(json.dumps(record,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    if record.get('diagnostic_session'):
        (out/'latest-diagnostic.json').write_text(json.dumps(record,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    print(record.get('stdout_tail',''), end='')
    print(record.get('stderr_tail',''), end='')
    print(f'Evidence: {destination}')
    return 0 if record.get('mapping_passed') or record.get('expected_stop') or (gpu and record.get('exit_code')==0 and not record.get('timeout')) else 1

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--entry', action='store_true', help='execute startup up to the first unsupported call')
    parser.add_argument('--linked', action='store_true', help='use native libc/Fios2 and diagnostic Windows TLS')
    parser.add_argument('--gpu',action='store_true',help='load the isolated Vulkan renderer')
    parser.add_argument('--timeout',type=int,default=10,help='deadline in seconds; 0 disables the deadline')
    parser.add_argument('--approved',action='store_true',help='use the validated city build and play settings')
    args = parser.parse_args()
    try:
        raise SystemExit(run(args.entry, args.linked,args.gpu,args.timeout,args.approved))
    except (OSError, ValueError, KeyError) as e:
        parser.exit(1, f'Probe blocked: {e}\n')
