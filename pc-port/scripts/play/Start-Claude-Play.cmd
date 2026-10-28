@echo off
setlocal
rem inFAMOUS - Claude build: persistent saves, shader cache grows each session (out\claude-play).
python "%~dp0..\play_claude.py"
exit /b %ERRORLEVEL%
