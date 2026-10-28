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

## Next step

1. On Windows, build the probe and run `prepare.py` then `link_modules.py` against the dump. Both
   hashes have to match before a run means anything.
2. Run to the first unimplemented import and record it here with its library and arguments. The
   stub's unnamed second libkernel import is a likely first stop, if it is not
   `sceKernelLoadStartModule`.

## Known unknowns

How the game behaves once it starts. Whether `libSceJobManager` can be satisfied by the runtime or has
to be linked from the dump; whether `libSceJson2` and the font libraries need work before the first
frame; the GPU entry points it uses and in what order; the memory layout it expects. None of this
should be assumed from another title.

## Diagnostics

Hang reports, dumps, `ISS_WATCH`, `PROBE_EVENT` and `frames.csv` came over with the runtime. None of
them has been produced for this game, and none of the offsets or hashes recorded against Ghost of
Tsushima apply here. `ISS_WATCH` takes addresses; it has no default site.
