# Status

Last updated 2026-10-28. Update this file whenever a run, a branch or a finding changes what the next
step is. Newest state first; keep the history short.

## Where the work stops

**The runtime is in the tree and the two-stage load is written. Nothing has been built or run.**
This environment has no Windows toolchain (no clang-cl, Visual Studio or CMake), so `iss-probe.exe`,
`iss-gpu.dll` and the launcher were not compiled, and no dump was linked or executed. The first
unimplemented import is still unknown. What was checked here is the Python linker
(`pc-port/tests/test_link_modules.py`, including the deferred-module flag) and the portable libc
tests (`libc_portable_test.c`).

## What the code does now

`pc-port/` is the got-pc-port runtime, renderer bridge, launcher, scripts and tests, with this
game's identity left as it was: both hashes in `game_id.py`, and `GAME_STUB_ENTRY` `0x4fa0`,
`GAME_MAIN_ENTRY` `0x80`, `GAME_MAIN_PATH` `/app0/game.elf` in `game.h`. Saves, trophies and the
GPU cache key off `GAME_TITLE_ID`.

`prepare.py`, `link_modules.py` and the launcher refuse a folder unless `eboot.bin` and `game.elf`
both match those hashes and those entry points, and unless `sce_module` has all three of `libc.prx`,
`libSceFios2.prx` and `libSceJobManager.prx`, or none of them. `boot.bin` is still only the stub.
`boot-linked.bin` contains the stub, the bundled modules and `game.elf`.

**Two-stage boot.** The stub is not disassembled here; the dump is not in this environment. The
measured facts are that it is 0x61a4 bytes, its entry is `+0x4fa0`, and it imports two libkernel
functions, one `libSceSystemService` function and 59 libc functions, with the string
`/app0/game.elf`. That is treated as `sceKernelLoadStartModule("/app0/game.elf")` (the backslash
form of the same path is accepted; any other path still returns "not available"). The second
libkernel import is not named in the inventory. If it is not this call, it still traps.

`game.elf` is parsed, relocated and import-bound by the existing linker, the same one that links
the PRXs, and its module record has flags bit 0 set. The probe therefore does not run its `DT_INIT`
or its entry while it is starting the PRXs. It does enter the stub at `+0x4fa0`. TLS for `game.elf`
is reserved with the other modules before that, so the block exists early; only the code waits.
When the stub calls `sceKernelLoadStartModule`, the runtime runs `game.elf`'s `DT_INIT` if the image
has one, then calls module base `+0x80` as `(args, argp, NULL)`. That is the libkernel module-start
contract, not a trace of the bytes at `+0x4fa0`. If that entry is actually a CRT `_start` with a
different frame, the run will fault, and that has not been tried. The SDK version handed to the GPU
is the stub's `sceProcParam`, not `game.elf`'s; linking fails if the stub does not have exactly one.
Whether the stub has that segment was not re-checked here.

Code-address patches from inFAMOUS and Ghost of Tsushima are not installed. `native_install_hooks`
returns immediately unless `ISS_LEGACY_HOOKS=1`, and the probe does not call it. The old save-name
call at `+0x1e50a0` is gone. The pause-guard counter and the two readback sites stay in the code and
stay off. `ISS_UNIFIED_RANGE` and `ISS_FAULT_WINDOW_KB` are not set: those numbers were measured on
the other game. Renderer fast paths that compare Ghost of Tsushima shader hashes are still in the
GPU code; they do not match this title and were not deleted.

## What the dump says (all of this measured, not assumed)

From `I:\More Stuff\Knack 2\CUSA08014`, 48.6 GB unpacked, `param.sfo` `TITLE_ID` `CUSA08014`,
`TITLE` `KNACK 2`, `CONTENT_ID` `UP9000-CUSA08014_00-KNACK20000000000`, `APP_VER` `01.00`.

**This game boots in two stages, and that is the biggest structural difference from the port this
one derives from.**

- `eboot.bin` is a 37,006-byte SELF that is nothing but a loader. Its only `LOAD` segment is
  0x61a4 bytes at vaddr 0, its entry point is `+0x4fa0`, and the strings in it are `/app0/game.elf`,
  `/app0/data/eboot.dat`, and the three modules it links: `libSceSystemService.prx`,
  `libkernel.prx`, `libc.prx`. It imports 62 functions (59 libc, 2 libkernel, 1 libSceSystemService).
  sha256 `e65b5fd0…3a4041db`.
- `game.elf` is the actual game: 20,205,618 bytes, entry `+0x80`, image ending at `0x1868160`, three
  `LOAD` segments (text at 0, a 0x4c000 read-only segment at `0x1200000`, and a 0x61c160 data segment
  at `0x124c000`). It is `MGame_Master_ORBIS.elf`; its strings still carry `D:\knack2\src\MGameSrc\...`
  debug paths. sha256 `80ccc228…d573fb20`.

**Import surface of `game.elf`: 778 imports across 43 libraries**, against `DT_NEEDED`'s 48 modules.
The shape matters more than the number:

| Library | Imports | Note |
| --- | --- | --- |
| `libc` | 192 | the game ships its own `libc.prx`, so these resolve against it first |
| `libSceGnmDriver` | 113 | the GPU command stream: this is what the renderer bridge has to answer |
| `libkernel` | 82 | |
| `libSceFios2` | 43 | file I/O; the game ships `libSceFios2.prx` |
| `libSceJson2` | 40 | new against the earlier ports; the game's data pipeline leans on it |
| `libSceFont` / `libSceFontFt` | 36 | text rendering, also new |
| `libSceNet` | 33 | |
| `libSceNpScore` | 19 | |
| `libSceAvPlayer` | 17 | |
| `libSceVideoOut` | 14 | |
| `libSceAudio3d` / `libSceNgs2` / `libSceAudioOut` | 30 | |
| `libSceJobManager` | 7 | the game ships `libSceJobManager.prx`; new against the earlier ports |
| everything else | 118 | dialogs, saves, trophies, PSN, PlayGo, Ime, Mouse, Fiber, PngDec, … |

Modules present in `sce_module`: `libc.prx` (1,069,730 B), `libSceFios2.prx` (446,777 B),
`libSceJobManager.prx` (59,154 B). All three are linked; none of them needs the runtime's stand-in
libc unless one is missing.

`data/eboot.dat` is 28 bytes: `-w =10 -cursoroff -region=a`. That is a launcher-tool leftover, not
something the port reads.

## Building it on Windows

Nothing below has been run, but every command is the one in the merged script named beside it, so a
failure here is a real bug in the script and not a typo in the notes.

**What the machine needs.** Windows 11, Visual Studio 2022 (or later) with *Desktop development with
C++* and *C++ CMake tools for Windows*, Python 3.12, and `clang-cl` at `tools/llvm/bin/clang-cl.exe`.
That path is hard-coded in `build.py` and `build_gpu.py`. `tools/` and `research/` are git-ignored, so
if got-pc-port is already checked out on the same machine, copy its `tools/` and its
`research/shadPS4-perf` across instead of downloading LLVM again.

**1. The renderer's base.** The renderer builds against shadPS4-perf at `584374867d6a`, which is the
default branch of the private mirror:

```bat
git clone -b port-base https://github.com/crisprintsstuff/shadPS4-perf research\shadPS4-perf
cd research\shadPS4-perf
git submodule update --init --recursive
git apply ..\..\pc-port\third_party\shadPS4-perf-local.patch
```

**2. The CMake dependencies.** From the repo root, in a shell where Visual Studio is discoverable
(`build_deps.cmd` finds it with vswhere, or set `VCVARS64` to a `vcvars64.bat`):

```bat
pc-port\scripts\build_deps.cmd
```

That yields `build-opt/` and `build-sdl/`. Both are needed: `build_gpu.py` reads the compile defines,
include paths and link libraries straight out of `build-opt/build.ninja`, and the probe links
`build-sdl/SDL3.dll`.

**3. Read and link the dump.** This only reads your files and writes under `pc-port/out/`, which is
git-ignored:

```bat
cd pc-port\scripts
python prepare.py --game "I:\More Stuff\Knack 2\CUSA08014"
python link_modules.py --game "I:\More Stuff\Knack 2\CUSA08014"
```

Both refuse the folder unless `eboot.bin` and `game.elf` match their hashes, both entry points are the
expected ones, and `param.sfo` names `CUSA08014`. `prepare.py` writes `boot.bin`, `imports.csv` and
`analysis.json`; `link_modules.py` writes `boot-linked.bin`, `linked-imports.csv` and `link.json`.
Only the stub is in `boot.bin`; `game.elf` is in `boot-linked.bin`.

**4. Build the probe and the renderer.**

```bat
python build.py
python build_gpu.py --jobs 8
```

`build.py` links `out/iss-probe.exe` and then compiles and runs the C tests (runtime, sync, pad, save
dialog, save search, portable libc, readback hint) before returning. `build_gpu.py` compiles the
shadPS4 sources plus `gpu/overrides` against clang-cl and links `out/iss-gpu.dll`; `--jobs` is capped
at 8, and `--dry-run` prints the whole configuration without compiling.

**5. Run it.** Put the game folder on the first line of `pc-port/out/game_dir.txt`, then from the repo
root:

```bat
pc-port\scripts\run_game.ps1
```

The script sets `ISS_GAME_DIR`, `ISS_GPU=1` and `ISS_TLS_PATCH=1`, runs
`iss-probe.exe boot-linked.bin --entry`, and writes `run.log` and `frames.csv` to the repo root.
`ISS_FAULT_WINDOW_KB` and `ISS_UNIFIED_RANGE` stay unset on purpose. Flags: `-HangReport <seconds>`
writes `hang-report.txt` if a frame stalls that long, `-Dump "+0xADDRESS:0xSIZE"`,
`-DumpReg "main:r13:0x200"`, `-TraceMspace`, `-IrqLog`, `-Profile`. Summarize a run with
`python pc-port\scripts\analyze_performance.py frames.csv`.

**The first run is a test of the second-stage guess, not of the game.** Three outcomes, each telling
you something different: it stops in the stub before the load call, meaning the stub does something
other than `sceKernelLoadStartModule`; it reaches the load and dies, meaning the module-start frame is
wrong; or it enters `game.elf` at `+0x80` and traps on an unimplemented import, which is the good
outcome and the number to record in *Next step*. `-HangReport 40` is the cheapest way to see which.

## Next step

1. On Windows, follow *Building it on Windows* above until `run.log` exists. Nothing in the repo has
   been compiled yet, so the build scripts themselves are unproven.
2. Record where the first run stops. If it reaches `game.elf`, write down the first unimplemented
   import with its library and arguments, and move this file's *Where the work stops* section onto
   that. If it stops before that, the guess about how the stub loads `game.elf` is wrong, and that
   becomes the next thing to look at.

## Known unknowns

How the game behaves once it starts. Whether `libSceJobManager` can be satisfied by the runtime or has
to be linked from the dump; whether `libSceJson2` and the font libraries need work before the first
frame; the GPU entry points it uses and in what order; the memory layout it expects. None of this
should be assumed from another title.

## Diagnostics

Hang reports, dumps, `ISS_WATCH`, `PROBE_EVENT` and `frames.csv` came over with the runtime. None of
them has been produced for this game, and none of the offsets or hashes recorded against Ghost of
Tsushima apply here. `ISS_WATCH` takes addresses; it has no default site.
