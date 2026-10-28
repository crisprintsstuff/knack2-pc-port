"""Focused tests for page provenance and the exact native arena initializer."""
import hashlib
import json
from pathlib import Path
import subprocess
from build_gpu import CLANG, environment

def main():
    root=Path(__file__).resolve().parents[1]
    out=root/'out'
    boot=(out/'boot-linked.bin').read_bytes()
    size=json.loads((out/'link.json').read_text())['image_size']
    start=len(boot)-size+0x577cb0
    if hashlib.sha256(boot[start:start+25]).hexdigest()!='e8f64174a5bb318f64218cff4a7fcb8874833661f56e0d232be3fb9720af32d1':
        raise ValueError('arena machine-code oracle differs from the prepared image')
    env=environment()
    for name,sources in (
        ('page-write-provenance-test', ['tests/page_write_provenance_test.cpp']),
        ('native-feedback-arena-test', ['tests/native_feedback_arena_test.cpp','native/feedback_arena.cpp'])):
        exe=out/(name+'.exe')
        subprocess.run([str(CLANG),'/nologo','/std:c++20','/EHsc','/W4','/WX','/O2','/MD',
            '/D_CRT_SECURE_NO_WARNINGS','/clang:-fuse-ld=lld',*[str(root/p) for p in sources],
            '/Fe:'+str(exe)],cwd=out,env=env,check=True)
        subprocess.run([str(exe)],cwd=out,check=True,timeout=20)

if __name__=='__main__':main()
