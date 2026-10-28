@echo off
setlocal
rem Isolated 120-second experiment. Does not alter the approved launcher or trusted cache.
set "ISS_PROBE_EXE=%~dp0..\..\out\releases\20261013-first-use-88-engine\iss-probe.exe"
set "ISS_PIPELINE_CACHE_SEED=%~dp0..\..\out\experimental\first-use-88-engine"
set "ISS_PIPELINE_CACHE_ROOT="
set "ISS_RING_BARRIERS=1"
set "ISS_REAL_OCCLUSION=1"
set "ISS_CP_SPLIT=0"
set "ISS_ASYNC_COMPUTE=0"
set "ISS_UPLOAD_WORKER=1"
set "ISS_RENDER_REUSE=1"
set "ISS_PARALLEL_RENDER=1"
set "ISS_DESCRIPTOR_FAST_PATH=1"
set "ISS_FAULT_WINDOW_KB=64"
set "ISS_TUNING=async_written_uploads=1,scout=1,scout_fast=1,bind_prefetch=1,tiered_compute_pipelines=0"
set "ISS_MAPPING_CACHE=1"
set "ISS_GPU_PROFILE=0"
set "ISS_MINIMAL_LOGS=0"
set "ISS_PIPELINE_EVENTS=0"
set "SHADPS4_PARALLEL_PRELOAD=1"
python "%~dp0..\run_probe.py" --linked --entry --gpu --timeout 120
exit /b %ERRORLEVEL%
