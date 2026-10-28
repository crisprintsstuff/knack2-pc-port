"""Package only shader diagnostic changes, preserving the original renderer."""
import hashlib
import json
from pathlib import Path
import subprocess

workspace=Path(__file__).resolve().parents[2]
baseline=workspace/'pc-port/evidence/codex-shaders-snapshot/gpu'
candidate=workspace/'pc-port-shaders/gpu'
original=workspace/'pc-port/gpu'
for p in baseline.rglob('*'):
    if p.is_file():
        assert p.read_bytes()==(original/p.relative_to(baseline)).read_bytes(), p
result=subprocess.run(['git','diff','--no-index','--',
    'pc-port/evidence/codex-shaders-snapshot/gpu','pc-port-shaders/gpu'],
    cwd=workspace,capture_output=True)
assert result.returncode==1, result.stderr
patch=result.stdout.decode('utf-8')
for prefix in ('a','b'):
    for directory in ('pc-port/evidence/codex-shaders-snapshot/gpu','pc-port-shaders/gpu'):
        patch=patch.replace(f'{prefix}/{directory}/',f'{prefix}/gpu/')
evidence=workspace/'pc-port/evidence/codex-shaders-patches'
(evidence/'phase0.patch').write_text(patch,encoding='utf-8',newline='\n')
files=[p for p in candidate.rglob('*') if p.is_file() and
       (not (baseline/p.relative_to(candidate)).exists() or
        p.read_bytes()!=(baseline/p.relative_to(candidate)).read_bytes())]
out=workspace/'pc-port/out/experimental/20261015-shaders-phase0'
files += [out/'iss-probe.exe',out/'iss-gpu.dll']
manifest={str(p.relative_to(workspace)):hashlib.sha256(p.read_bytes()).hexdigest() for p in files}
(evidence/'phase0-sha256.json').write_text(json.dumps(manifest,indent=2),encoding='utf-8')
print(json.dumps({'original_gpu_unchanged':True,'manifest':manifest},indent=2))
