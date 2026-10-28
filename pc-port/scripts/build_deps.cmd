@echo off
rem Builds the CMake dependencies of the port, from any folder:
rem   build-opt  shadPS4-perf with clang-cl (the renderer's compile flags, zydis, fmt and the
rem              other libraries build_gpu.py and build.py link)
rem   build-sdl  SDL3 (SDL3.dll and SDL3.lib)
rem Needs research\shadPS4-perf (patched), tools\llvm and Visual Studio with the C++ workload and
rem "C++ CMake tools for Windows". VCVARS64=<path to vcvars64.bat> picks a Visual Studio.
setlocal
set "ROOT=%~dp0..\.."
if not defined VCVARS64 (
  for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VCVARS64=%%i\VC\Auxiliary\Build\vcvars64.bat"
)
if not exist "%VCVARS64%" (echo No Visual Studio with "Desktop development with C++" found. & exit /b 1)
call "%VCVARS64%" >nul || exit /b 1
set "PATH=%ROOT%\tools\llvm\bin;%PATH%"
if not exist "%ROOT%\tools\llvm\bin\clang-cl.exe" (echo Missing tools\llvm\bin\clang-cl.exe & exit /b 1)
if not exist "%ROOT%\research\shadPS4-perf\CMakeLists.txt" (echo Missing research\shadPS4-perf & exit /b 1)
where cmake >nul 2>nul || (echo CMake not found: install "C++ CMake tools for Windows" in Visual Studio. & exit /b 1)
where ninja >nul 2>nul || (echo Ninja not found: install "C++ CMake tools for Windows" in Visual Studio. & exit /b 1)
set "COMMON=-G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_POLICY_VERSION_MINIMUM=3.5"
rem LibreSSL defines be64toh/htole32 itself on Windows unless CMake finds an endian.h; a newer
rem toolchain has one without those functions, so the check is answered "no".
cmake -S "%ROOT%\research\shadPS4-perf" -B "%ROOT%\build-opt" %COMMON% -DHAVE_ENDIAN_H=OFF -DHAVE_MACHINE_ENDIAN_H=OFF || exit /b 1
cmake --build "%ROOT%\build-opt" || exit /b 1
cmake -S "%ROOT%\research\shadPS4-perf\externals\sdl3" -B "%ROOT%\build-sdl" %COMMON% -DSDL_TEST_LIBRARY=OFF || exit /b 1
cmake --build "%ROOT%\build-sdl" || exit /b 1
if not exist "%ROOT%\build-opt\externals\zydis\Zydis.lib" (echo build-opt has no externals\zydis\Zydis.lib & exit /b 1)
if not exist "%ROOT%\build-sdl\SDL3.dll" (echo build-sdl has no SDL3.dll & exit /b 1)
echo Dependencies built: build-opt and build-sdl.
