"""User-driven diagnostic session; approved seeds, isolated saves, no experimental paths."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import time
import run_probe

ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'out'
RELEASE=OUT/'releases/20261013-diagnostico-frame'
PACK=OUT/'pipeline-library/CUSA00004-packed'
TRUSTED=OUT/'pipeline-library-runtime-trusted'
MANIFEST_SHA='c3f63382830107fc0221c24dd3376533807a73357ec1407399531c1e9b815e66'

def sha(path):
    with path.open('rb') as source:return hashlib.file_digest(source,'sha256').hexdigest()

def verify():
    build=json.loads((RELEASE/'diagnostic-build.json').read_text(encoding='utf-8'))
    for name,digest in build['binaries'].items():
        if sha(RELEASE/name)!=digest:raise ValueError(f'Build de diagnóstico alterada: {name}')
    if sha(PACK/'manifest.json')!=MANIFEST_SHA:raise ValueError('O pack aprovado mudou')
    profile=TRUSTED/'CUSA00004/profile.bin'
    if not profile.is_file():raise FileNotFoundError('Cache aprovado ausente')
    saves=OUT/'user/city-savedata'
    if not list(saves.rglob('dis.dat')):raise FileNotFoundError('Save base ausente')
    return {'build':build,'pack_manifest_sha256':MANIFEST_SHA,
        'trusted_profile_sha256':sha(profile),
        'save_seed_hashes':{str(p.relative_to(saves)):sha(p) for p in saves.rglob('dis.dat')}}

def host_context():
    result={'platform':platform.platform(),'logical_processors':os.cpu_count(),
            'python':sys.version,'monotonic_ns':time.perf_counter_ns()}
    command="""$ErrorActionPreference='Stop'; [ordered]@{
cpu=@(Get-CimInstance Win32_Processor | Select-Object Name,NumberOfCores,NumberOfLogicalProcessors,MaxClockSpeed);
gpu=@(Get-CimInstance Win32_VideoController | Select-Object Name,DriverVersion,DriverDate,CurrentHorizontalResolution,CurrentVerticalResolution,CurrentRefreshRate);
ram=(Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory;
power_plan=(powercfg /getactivescheme | Out-String)
} | ConvertTo-Json -Depth 5 -Compress"""
    try:
        run=subprocess.run(['powershell.exe','-NoProfile','-NonInteractive','-Command',command],
            capture_output=True,text=True,timeout=20,creationflags=subprocess.CREATE_NO_WINDOW)
        result['hardware']=json.loads(run.stdout) if run.returncode==0 else {'error':run.stderr[-2000:]}
    except Exception as error:result['hardware_error']=str(error)
    return result

def stop_session():
    pointer=OUT/'latest-diagnostic.json'
    if not pointer.is_file():print('Nenhuma sessão de diagnóstico registrada.');return 0
    record=json.loads(pointer.read_text(encoding='utf-8'))
    run_dir=Path(record['run_dir']).resolve()
    if not run_dir.is_relative_to((OUT/'runs').resolve()) or not record.get('diagnostic_session'):
        raise ValueError('Referência de diagnóstico inválida')
    if 'exit_code' in record:print('A última sessão já foi encerrada.');return 0
    (run_dir/'stop.request').touch()
    print('Encerramento solicitado. Aguarde o jogo fechar e os logs terminarem.');return 0

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check',action='store_true',help='Verify files without starting the game')
    parser.add_argument('--stop',action='store_true')
    args=parser.parse_args()
    if args.stop:return stop_session()
    proof=verify()
    if args.check:
        print(json.dumps({'ready':True,'release':str(RELEASE),'verification':proof},indent=2));return 0
    import msvcrt
    with (OUT/'diagnostic-session.lock').open('a+b') as lock:
        lock.seek(0)
        try:msvcrt.locking(lock.fileno(),msvcrt.LK_NBLCK,1)
        except OSError:raise RuntimeError('Já existe uma sessão de diagnóstico aberta.')
        system=host_context()
        # Do not inherit a shell's old experiment or cache override.
        for key in list(os.environ):
            if key.startswith(('ISS_','SHADPS4_')):os.environ.pop(key,None)
        run_probe.apply_approved_environment(os.environ,OUT)
        os.environ.update(
            ISS_PROBE_EXE=str(RELEASE/'iss-probe.exe'),ISS_SAVE_SEED=str(OUT/'user/city-savedata'),
            ISS_DIAGNOSTIC_SESSION='1',ISS_TIMELINE='1',ISS_GPU_PROFILE='1',
            ISS_MINIMAL_LOGS='0',ISS_PIPELINE_EVENTS='1',
            ISS_NATIVE_GEOMETRY='0',ISS_NATIVE_DRAW_STATE='0',ISS_NATIVE_DRAW_STATE_VERIFY='0',
            ISS_NATIVE_INDEXED_BATCH='0',ISS_NATIVE_INSTANCED_BATCH='0',ISS_NATIVE_INSTANCED_FEATURE='0',
            ISS_NATIVE_READBACK_PAGES='0',ISS_NATIVE_CITY_FETCH='0',ISS_DRAW_PIPE='0',
            ISS_PROFILE_CITY_DRAW_GPU='0',ISS_DEEP_PROFILE='0',ISS_CAPTURE_FRAMES='0',
            ISS_COMMAND_TRACE='0',ISS_FEEDBACK_JOIN='0',ISS_READBACK_DIAG='0')
        print('Diagnóstico pronto para jogar. F10 marca uma travada; F9 mostra/oculta FPS.',flush=True)
        print('Jogue normalmente por alguns minutos. Feche o jogo para concluir a coleta.',flush=True)
        status=run_probe.run(entry=True,linked=True,gpu=True,timeout=0,isolate_saves=True,
            diagnostic_context={'verification':proof,'system':system})
        record=json.loads((OUT/'latest-diagnostic.json').read_text(encoding='utf-8'))
        run_dir=Path(record['run_dir'])
        (run_dir/'diagnostic-context.json').write_text(json.dumps(
            {'verification':proof,'system':system,'analysis_completed_ns':time.perf_counter_ns()},indent=2),encoding='utf-8')
        errors=[]
        try:
            from analyze_runtime_timeline import analyze
            analyze(run_dir)
        except Exception as error:errors.append('timeline: '+str(error))
        try:
            from analyze_performance import summarize
            summarize(run_dir/'frames.csv')
        except Exception as error:errors.append('frames: '+str(error))
        (run_dir/'diagnostic-analysis-status.json').write_text(json.dumps({'errors':errors},indent=2),encoding='utf-8')
        print('\nLogs salvos em: '+str(run_dir),flush=True)
        return 0 if record.get('exit_code')==0 and not record.get('corruption_dialog') else status or 1

if __name__=='__main__':
    try:raise SystemExit(main())
    except (OSError,ValueError,RuntimeError,KeyError) as error:
        print('Diagnóstico não iniciado: '+str(error),file=sys.stderr);raise SystemExit(1)
