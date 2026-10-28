# SPDX-License-Identifier: GPL-2.0-or-later
"""Builds the launcher (launcher/launcher.cpp) with the toolchain of build.py:
python scripts/build_launcher.py [--output PATH]"""
import argparse
import os
from pathlib import Path
import subprocess
import sys

from vs_env import environment

ROOT = Path(__file__).resolve().parents[1]
# CLANG_CL=<path to clang-cl.exe> overrides the repo's own LLVM (CI uses the runner's).
CLANG = Path(os.environ.get('CLANG_CL') or ROOT.parent / 'tools/llvm/bin/clang-cl.exe')


def build(output):
    env = environment(CLANG.parent)
    output.parent.mkdir(parents=True, exist_ok=True)
    work = ROOT / 'out' / 'launcher-build'
    work.mkdir(parents=True, exist_ok=True)
    cmd = [str(CLANG), '/nologo', '/std:c++20', '/EHsc', '/W4', '/O2', '/MT', '/DUNICODE', '/D_UNICODE',
           '/D_CRT_SECURE_NO_WARNINGS', str(ROOT / 'launcher' / 'launcher.cpp'),
           str(ROOT / 'launcher' / 'prepare_game.cpp'), f'/Fo:{work}\\',
           f'/Fe:{output}', '/clang:-fuse-ld=lld', '/link', '/SUBSYSTEM:WINDOWS',
           'user32.lib', 'gdi32.lib', 'gdiplus.lib', 'shell32.lib', 'ole32.lib', 'dwmapi.lib',
           'shlwapi.lib', 'uuid.lib', 'bcrypt.lib']
    subprocess.run(cmd, cwd=work, env=env, check=True)
    print(f'Built {output}')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', type=Path,
                        # The repository root: from a checkout the launcher finds the port built in
                        # pc-port/out, the prepared image and game_dir.txt (launcher.cpp, g_repo).
                        default=ROOT.parent / 'Knack II Launcher.exe')
    args = parser.parse_args()
    try:
        build(args.output.resolve())
    except (OSError, subprocess.CalledProcessError) as e:
        sys.exit(f'Build failed: {e}')
