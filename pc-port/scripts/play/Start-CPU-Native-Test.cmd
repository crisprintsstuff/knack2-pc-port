@echo off
setlocal
rem Visual control while investigating reported pop-in. Experimental CPU paths are off.
python "%~dp0..\run_cpu_native.py" --seconds 120 --approved
exit /b %ERRORLEVEL%
