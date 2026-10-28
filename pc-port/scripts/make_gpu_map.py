"""Relink the current iss-gpu.dll objects with /MAP into out/gpu-map and verify identical layout.

The object order of build_gpu.py changes every build: run this after each build, before
profile_threads.py --map out/gpu-map/iss-gpu.map."""
import pathlib, subprocess, sys
PORT = pathlib.Path(r'C:\Games\Infamous Second Son PC Port\Infamous Second Son Optmize\pc-port')
sys.path.insert(0, str(PORT / 'scripts'))
import build_gpu
S = PORT / 'out/gpu-map'
S.mkdir(exist_ok=True)
r = (PORT / 'out/gpu-build/link.rsp').read_text()
real = str(build_gpu.ROOT / 'out/iss-gpu.dll'); assert real in r
r = r.replace(real, str(S / 'iss-gpu.dll')) + ' /MAP:"' + str(S / 'iss-gpu.map') + '"'
(S / 'link.rsp').write_text(r)
res = subprocess.run([str(build_gpu.CLANG), '@' + str(S / 'link.rsp')], env=build_gpu.environment(),
                     cwd=build_gpu.WORKSPACE / 'build-opt', capture_output=True, text=True)
a = (S / 'iss-gpu.dll').read_bytes(); b = (PORT / 'out/iss-gpu.dll').read_bytes()
diff = [i for i in range(len(a)) if a[i] != b[i]] if len(a) == len(b) else ['size']
ok = res.returncode == 0 and all(isinstance(i, int) and 128 <= i <= 135 for i in diff)
print('map', 'OK' if ok else 'MISMATCH', diff[:6])
sys.exit(0 if ok else 1)
