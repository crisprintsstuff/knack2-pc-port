# SPDX-License-Identifier: GPL-2.0-or-later
"""The MSVC build environment (Windows SDK, CRT libraries) for clang-cl.

Any Visual Studio 2022 or later edition works, Build Tools included: vswhere finds the newest one
with the x64 C++ tools. VCVARS64=<path to vcvars64.bat> overrides the search.
"""
import os
from pathlib import Path
import subprocess

VSWHERE = Path(os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)')) / 'Microsoft Visual Studio/Installer/vswhere.exe'


def vcvars64():
    if os.environ.get('VCVARS64'):
        path = Path(os.environ['VCVARS64'])
        if not path.is_file():
            raise RuntimeError(f'VCVARS64 does not exist: {path}')
        return path
    if VSWHERE.is_file():
        found = subprocess.run([str(VSWHERE), '-latest', '-products', '*', '-requires',
                                'Microsoft.VisualStudio.Component.VC.Tools.x86.x64',
                                '-property', 'installationPath'],
                               capture_output=True, text=True, check=True).stdout.strip()
        if found:
            path = Path(found.splitlines()[0]) / 'VC/Auxiliary/Build/vcvars64.bat'
            if path.is_file():
                return path
    raise RuntimeError('No Visual Studio with "Desktop development with C++" found; '
                       'install it or set VCVARS64 to vcvars64.bat')


def environment(clang_dir):
    """os.environ plus the vcvars64 variables, with clang_dir first on PATH."""
    result = subprocess.run(f'cmd.exe /d /s /c ""{vcvars64()}" >nul && set"',
                            capture_output=True, text=True, check=True)
    env = os.environ.copy()
    for line in result.stdout.splitlines():
        if '=' in line:
            key, value = line.split('=', 1)
            env[key] = value
    env['PATH'] = str(clang_dir) + os.pathsep + env.get('Path', env.get('PATH', ''))
    env.pop('Path', None)
    return env
