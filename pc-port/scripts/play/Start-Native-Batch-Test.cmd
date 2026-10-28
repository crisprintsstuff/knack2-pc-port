@echo off
setlocal
rem Bounded native CPU/GPU indexed batch. Approved shader pack and trusted seed only.
python "%~dp0..\run_cpu_native.py" --batch --seconds 120
exit /b %ERRORLEVEL%
