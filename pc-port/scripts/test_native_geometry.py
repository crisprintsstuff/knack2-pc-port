"""Compare the CPU geometry port with the complete original EBOOT function."""
import hashlib
import json
from pathlib import Path
import subprocess
from build_gpu import environment, CLANG

ROOT = Path(__file__).resolve().parents[1]

def run():
    out = ROOT / 'out'
    image = (out / 'boot-linked.bin').read_bytes()
    size = json.loads((out / 'link.json').read_text(encoding='utf-8'))['image_size']
    begin = len(image) - size + 0x1a1960
    original = image[begin:begin + 549]
    if hashlib.sha256(original).hexdigest() != '58986629ee8c98fecc5411a70d91ff9e3952938d71d4aa7e1f75f18670958333':
        raise ValueError('unexpected original geometry emitter identity')
    (out / 'geometry-original.bin').write_bytes(original)
    exe = out / 'native-geometry-test.exe'
    subprocess.run([str(CLANG), '/nologo', '/std:c++20', '/EHsc', '/W4', '/WX', '/O2',
                    '/MD', '/arch:AVX2', '/D_CRT_SECURE_NO_WARNINGS', '/clang:-fuse-ld=lld',
                    str(ROOT / 'tests/native_geometry_test.cpp'),
                    str(ROOT / 'native/geometry.cpp'),
                    str(ROOT / 'native/geometry_hook.cpp'), f'/Fe:{exe}'],
                   cwd=out, env=environment(), check=True)
    subprocess.run([str(exe)], cwd=out, check=True, timeout=20)

if __name__ == '__main__':
    run()
