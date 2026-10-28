from pathlib import Path
import subprocess
import build_gpu
root=Path(__file__).resolve().parents[1]; work=root.parent
out=root/'out/experimental/20261015-shaders-backend-probe';out.mkdir(parents=True,exist_ok=True)
cmd=[str(build_gpu.CLANG),'/nologo','/std:c++20','/EHsc','/O2','/MD',
 '/I'+str(work/'research/shadPS4-perf/externals/vulkan-headers/include'),
 str(work/'pc-port-shaders/scripts/shader_backend_probe.cpp'),'/Fe:'+str(out/'probe.exe')]
subprocess.run(cmd,cwd=out,env=build_gpu.environment(),check=True)
