# SPDX-License-Identifier: GPL-2.0-or-later
"""Run the current milestones and retain exact command output as local evidence."""
from datetime import datetime
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
WORKSPACE = ROOT.parent

def validate():
    commands = [
        ('prepare', [sys.executable, 'pc-port/scripts/prepare.py']),
        ('link', [sys.executable, 'pc-port/scripts/link_modules.py']),
        ('build_and_native_contract_tests', [sys.executable, 'pc-port/scripts/build.py']),
        ('python_tests', [sys.executable, '-m', 'unittest', 'discover', '-s', 'pc-port/tests', '-v']),
        ('baseline_map', [sys.executable, 'pc-port/scripts/run_probe.py']),
        ('baseline_entry', [sys.executable, 'pc-port/scripts/run_probe.py', '--entry']),
        ('linked_map', [sys.executable, 'pc-port/scripts/run_probe.py', '--linked']),
        ('linked_init', [sys.executable, 'pc-port/scripts/run_probe.py', '--linked', '--entry']),
        ('original_fingerprints', [sys.executable, 'tools/check_integrity.py']),
    ]
    report = {'timestamp': datetime.now().astimezone().isoformat(), 'steps': [],
              'meaning': 'Current prototype/expected-stop validation; not successful game boot or gameplay.'}
    destination = ROOT / 'out/validation.json'
    destination.parent.mkdir(exist_ok=True)
    for name, command in commands:
        print(f'Checking {name}...', flush=True)
        try:
            result = subprocess.run(command, cwd=WORKSPACE, capture_output=True, text=True, timeout=60)
            row = {'name': name, 'command': command, 'exit_code': result.returncode,
                   'stdout': result.stdout, 'stderr': result.stderr, 'passed': result.returncode == 0}
        except (subprocess.TimeoutExpired, OSError) as error:
            row = {'name': name, 'command': command, 'passed': False, 'error': str(error)}
        report['steps'].append(row)
        report['all_passed'] = all(step['passed'] for step in report['steps'])
        destination.write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
        if not row['passed']:
            print(json.dumps(row, ensure_ascii=False, indent=2))
            return 1
    print(f'PASS: {len(commands)} validation steps. Evidence: {destination}')
    return 0

if __name__ == '__main__':
    raise SystemExit(validate())
