"""Run one isolated diagnostic, refusing concurrent games."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser()
p.add_argument('label')
a = p.parse_args()
processes = subprocess.check_output(['tasklist', '/FI', 'IMAGENAME eq iss-probe.exe', '/FO', 'CSV'], text=True)
if 'iss-probe.exe' in processes.lower():
    raise SystemExit('Another iss-probe.exe is open; no game was started.')
lineage = ROOT / 'out' / ('codex-cache-empty-' + a.label)
seed = lineage / 'current/CUSA00004'
seed.mkdir(parents=True, exist_ok=False)
shutil.copy2(ROOT / 'out/pipeline-cache-claude-dev/current/CUSA00004/profile.bin', seed)
flags = '''ISS_CLAMP_CACHE_FIRST=1 ISS_STAGE_MEMO=1 ISS_FAST_DRAW=1 ISS_FAST_REBIND=1
ISS_FAST_LEAN=1 ISS_FAST_SWITCH=1 ISS_SUBMIT_EVERY=1000 BB_VBLANK_HZ=240
ISS_COMPUTE_FAIRNESS=1 ISS_CP_CPU=14 ISS_STAGE_MEMO_LOOSE=1 ISS_STAGE_MEMO_TEXEL=1
ISS_LAZY_PIPELINES=1 ISS_SPEC_TRANSLATE=1 ISS_SHADER_SPIKES=1'''.split()
command = [sys.executable, str(ROOT / 'scripts/run_controlled_benchmark.py'),
    '--exe', str(ROOT / 'out/experimental/20261015-shaders-phase0/iss-probe.exe'),
    '--label', a.label, '--seconds', '90', '--cache-lineage', str(lineage)]
for flag in flags:
    command += ['--set', flag]
log = ROOT / 'out/experimental/20261015-shaders-phase0' / (a.label + '.log')
with log.open('w', encoding='utf-8') as f:
    result = subprocess.run(command, cwd=ROOT, stdout=f, stderr=subprocess.STDOUT)
print(json.dumps({'log': str(log), 'returncode': result.returncode}))
raise SystemExit(result.returncode)
