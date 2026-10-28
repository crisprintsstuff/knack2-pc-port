# Upstream: the Ghost of Tsushima port's README

Archived 2026-10-28 from the root `README.md` of
[crisprintsstuff/got-pc-port](https://github.com/crisprintsstuff/got-pc-port), the port this project
starts from. Kept verbatim because its build steps, dependency commit and diagnostics are what this
project inherits. Its status lines, its title id and its measured settings describe that game, not
this one.

---

# Ghost of Tsushima PC port (CUSA11456)

A native Windows runtime for the PS4 version of Ghost of Tsushima, forked from
[Castlesea/infamous-second-son-pc-port](https://github.com/Castlesea/infamous-second-son-pc-port).
The game's own x86-64 code runs directly on the host CPU; a custom runtime replaces the PS4
system libraries, and graphics go through a Vulkan renderer derived from shadPS4.

**Status: reaches the game.** On Windows with an NVIDIA RTX 3060 the game boots, shows its
menus in English and plays the opening cutscene at its 30 FPS cap. Gameplay past the cutscene is
the next thing being tested. Details and open problems: [`status.md`](../STATUS.md).

No game files, keys or copyrighted assets are part of this repository. You need a decrypted
dump of your own copy: the retail eboot.bin from patch 2.18 (sha256 `4980b598…91afbd`).
The game identity lives in `pc-port/scripts/game_id.py` and `pc-port/src/game.h`.

### Why a .pkg is not enough

The port reads the unpacked game folder (`eboot.bin`, `sce_sys/`, `sce_module/`), and the
`eboot.bin` in it must be decrypted. A retail `.pkg` stays encrypted for the console it is
licensed to, and the executables inside are encrypted again. Only that console can decrypt them,
so the port cannot use a retail `.pkg` directly. Dump the installed game from your own PS4
instead. The launcher says so when it is given a folder that holds the game's `.pkg` and no
`eboot.bin`. DLC `.pkg` files are different: the launcher reads only their content ids, to unlock
the DLC.

Current state, open branches and the next step: `docs/STATUS.md`. Working in the
repo (people or coding agents): `AGENTS.md`.

## Differences from the inFAMOUS port

- libc: like inFAMOUS, the game's own `sce_module/libc.prx` and `libSceFios2.prx` are linked
  when the dump has them. Without them the runtime's libc stands in for all 120 libc imports:
  `libc.c` (host CRT wrappers), `libc_format.c` + `libc_varargs.S` (printf/scanf over SysV
  argument lists) and `libc_mspace.c` (sceLibcMspace allocators inside the game's memory ranges).
  `tests/libc_portable_test.c` also runs on Linux. `libSceNpToolkit2.prx` is never linked.
- PSN, voice chat, sharing, streaming, crash dumps and sockets behave as on a console that is
  signed out with no network (`offline.c`); each logs `OFFLINE <api>` on its first call.
- The inFAMOUS native hooks (`pc-port/native/`, and the eboot patches in `gpu/bridge.cpp`) patch
  inFAMOUS code addresses and are not installed.
- The inFAMOUS shader packs, recipes and profiling scripts are kept for reference but do not apply.

## Roadmap

1. ~~Retarget identity checks, launcher and save paths~~
2. ~~libc replacement and offline stubs~~ (`pc-port/src/libc*.c`, `pc-port/src/offline.c`; untested on Windows)
3. ~~Build on Windows; run to the first unimplemented call~~
4. The remaining imports: 55 of the eboot (Audio3d, Rtc, save data memory, trophy info, kernel event
   queues and memory) and 57 of libc.prx/Fios2 (kernel: flexible memory, rwlocks, atexit, time)
5. ~~Renderer bring-up~~ (menus and the opening cutscene at 30 FPS)
6. Gameplay

## Preparing (any OS with Python 3)

    python pc-port/scripts/prepare.py --game "<game folder>"
    python pc-port/scripts/link_modules.py --game "<game folder>"

To run a build, put the path of your game folder on the first line of `pc-port/out/game_dir.txt`,
then either:

- **The launcher:** `python pc-port/scripts/build_launcher.py` builds
  `Ghost of Tsushima Launcher.exe` in the repository root (or take it from the Launcher workflow's
  artifact and put it there). From a checkout it uses the port built in `pc-port\out`, the
  prepared image there and `game_dir.txt`; its settings and logs go to `user\` next to it.
- **The script:** `Launch.cmd`, or `pc-port\scripts\run_game.ps1` for its debugging options
  (`-HangReport`, `-Dump`, `-DumpReg`, `-TraceMspace`, `-IrqLog`, `-Profile`). The output goes
  to `run.log` and per-frame timings to `frames.csv` (`python pc-port/scripts/analyze_performance.py frames.csv`).

Both run with the settings the game was tested with: unified memory on the game's per-frame heap
(`ISS_UNIFIED_MEMORY`, `ISS_UNIFIED_RANGE=0x1500000000:0x56e00000`) and a 64 KB fault window.

Build steps (Windows 11, clang-cl, CMake, a shadPS4-perf checkout) are unchanged from upstream;
see `docs/UPSTREAM-INFAMOUS-README.md`. Any Visual Studio 2022 or later with "Desktop development
with C++" provides the Windows SDK; the scripts find it with vswhere (or set `VCVARS64`).

## Renderer sources

The renderer builds from shadPS4-perf at `584374867d6a`, a commit no longer on any branch upstream.
A private mirror keeps it (branch `port-base`):

    git clone -b port-base https://github.com/crisprintsstuff/shadPS4-perf research/shadPS4-perf
    cd research/shadPS4-perf
    git submodule update --init --recursive
    git apply ../../pc-port/third_party/shadPS4-perf-local.patch

Then `pc-port\scripts\build_deps.cmd` configures and builds `build-opt` (shadPS4-perf with
clang-cl: the renderer's flags and libraries) and `build-sdl` (SDL3).

## License

GPL-2.0-or-later, as upstream. See `pc-port/LICENSE` and `pc-port/THIRD_PARTY.md`.
Ghost of Tsushima is a trademark of Sony Interactive Entertainment. This project is not
affiliated with Sony or Sucker Punch Productions.
