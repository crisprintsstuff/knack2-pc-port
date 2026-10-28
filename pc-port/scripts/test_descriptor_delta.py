"""Test incremental descriptor updates against replaying complete descriptor batches."""
from pathlib import Path
import subprocess
from build_gpu import environment, CLANG, ROOT

out=ROOT/'out/tests/descriptor-delta'
out.mkdir(parents=True,exist_ok=True)
exe=out/'descriptor-delta-test.exe'
subprocess.run([str(CLANG),'/nologo','/std:c++20','/EHsc','/O2','/MD',
    f'/I{ROOT/"gpu"}',str(ROOT/'tests/descriptor_delta_test.cpp'),
    f'/Fo:{out/"test.obj"}',f'/Fe:{exe}','/clang:-fuse-ld=lld'],env=environment(),check=True)
subprocess.run([str(exe)],check=True)
