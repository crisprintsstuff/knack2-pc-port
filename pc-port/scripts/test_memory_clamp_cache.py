"""Test the production memory_clamp without allocating guest memory or starting a game."""
import itertools
import json
import os
from pathlib import Path
import subprocess
from build_gpu import environment, CLANG

ROOT = Path(__file__).resolve().parents[1]

def main():
    out = ROOT / 'evidence/clamp-cache-first-20261013'
    out.mkdir(parents=True, exist_ok=True)
    executable = out / 'memory-clamp-cache-test.exe'
    subprocess.run([str(CLANG), '/nologo', '/TC', '/std:c11', '/W4', '/WX', '/O2', '/MD',
                    '/D_CRT_SECURE_NO_WARNINGS', '/clang:-fuse-ld=lld',
                    str(ROOT / 'tests/memory_clamp_cache_test.c'),
                    f'/Fe:{executable}', f'/Fo:{out / "memory-clamp-cache-test.obj"}'],
                   cwd=out, env=environment(), check=True)
    results = []
    for first, index, cache in itertools.product(('0', '1'), repeat=3):
        env = dict(os.environ, ISS_CLAMP_CACHE_FIRST=first, ISS_MAPPING_INDEX=index,
                   ISS_MAPPING_CACHE=cache)
        result = subprocess.run([str(executable)], env=env, cwd=out,
                                capture_output=True, text=True, timeout=30, check=True)
        item = dict(cache_first=first, mapping_index=index, mapping_cache=cache,
                    output=result.stdout.strip())
        results.append(item)
        print(json.dumps(item), flush=True)
    (out / 'results.json').write_text(json.dumps(results, indent=2), encoding='utf-8')

if __name__ == '__main__':
    main()
