@echo off
rem Runs the game with the port's settings (pc-port\scripts\run_game.ps1). Needs a build in
rem pc-port\out and the game folder on the first line of pc-port\out\game_dir.txt.
if not exist "%~dp0pc-port\out\iss-probe.exe" (
    echo pc-port\out\iss-probe.exe is missing: build the port first ^(see README.md^).
    pause
    exit /b 1
)
if not exist "%~dp0pc-port\out\game_dir.txt" (
    echo pc-port\out\game_dir.txt is missing: put the path of your decrypted game dump on its first line.
    pause
    exit /b 1
)
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0pc-port\scripts\run_game.ps1" %*
pause
