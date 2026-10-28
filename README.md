# Knack II PC port (CUSA08014)

A native Windows runtime for the PS4 version of *Knack II*, built the same way as
[crisprintsstuff/got-pc-port](https://github.com/crisprintsstuff/got-pc-port): the game's own
x86-64 code runs directly on the host CPU, a custom runtime replaces the PS4 system libraries, and
graphics go through a Vulkan renderer derived from shadPS4.

**Status: nothing has run yet.** This repository is the starting point, not a working port. A
decrypted dump has been inspected and its identity recorded below, but no image has been prepared, no
build exists and the game has not been executed. What exists today is the identity of the target, the
rules for working here, what the dump turned out to contain, and a record of what carries over from
the Ghost of Tsushima port and what has to be redone from scratch.

No game files, keys or copyrighted assets are part of this repository. You need a decrypted dump of
your own copy.

## Which build

Knack II shipped in several regions, each its own eboot:

| Region | Title ID |
| --- | --- |
| US | `CUSA08014` |
| Europe / Australia | `CUSA07670` |
| Asia | `CUSA08095` |
| Japan (*Knack: Futari no Eiyuu to Kodai Heidan*) | `CUSA04774` |

The port targets **`CUSA08014`** until a different build is reviewed. As in the Ghost of Tsushima
port, the identity lives in one place per language — `pc-port/scripts/game_id.py` and
`pc-port/src/game.h` — and other builds are refused rather than half-supported. If your own copy is
one of the other three, say so before anything is built against it: the eboot hash, the import
inventory and every code offset differ.

This dump has been checked, and it is the right one: `param.sfo` reports `TITLE_ID` `CUSA08014`,
`TITLE` `KNACK 2`, `CONTENT_ID` `UP9000-CUSA08014_00-KNACK20000000000`, `APP_VER` `01.00`.

### This build boots in two stages

Unlike the Ghost of Tsushima build this port derives from, Knack II splits its executable in two:

- **`eboot.bin`** — 37,006 bytes. A loader and nothing else: one 0x61a4-byte code segment at vaddr 0,
  entry `+0x4fa0`, and the strings `/app0/game.elf`, `/app0/data/eboot.dat`, `libSceSystemService.prx`,
  `libkernel.prx`, `libc.prx`. It imports 62 functions. sha256
  `e65b5fd039833ada7dddd8c197b8b47c283a3f46320cd13ece5ff3973a4041db`.
- **`game.elf`** — 20,205,618 bytes, the actual game. Entry `+0x80`, image ending at `0x1868160`.
  It is `MGame_Master_ORBIS.elf`, and its strings still carry the original build paths
  (`D:\knack2\src\MGameSrc\...`). sha256
  `80ccc22819fa4eeb5e1824e585f3ac5d5168e9ebc82f629e44cdb63fd573fb20`.

Both hashes are pinned in `pc-port/scripts/game_id.py`, because the stub alone says nothing about
which game actually runs. The stub has to be linked first, and then *its* load of `/app0/game.elf`
has to reach the game's entry point at `+0x80` — that second stage has no equivalent in the earlier
ports and is the first piece of genuinely new code here.

### What the game asks for

`game.elf` imports **778 functions across 43 libraries**. The heavy ones are `libc` (192),
`libSceGnmDriver` (113, the GPU command stream), `libkernel` (82) and `libSceFios2` (43).
Beyond that it wants subsystems the earlier ports never had to answer: `libSceJson2` (40),
`libSceFont` and `libSceFontFt` (36 between them), `libSceNpScore` (19), `libSceAvPlayer` (17).
Its `sce_module` folder ships `libc.prx`, `libSceFios2.prx` and `libSceJobManager.prx`, all three of
which this port links. `libSceJobManager` is new against the earlier ports: the game's job system
goes through it.

## Why a .pkg is not enough

The port reads the unpacked game folder (`eboot.bin`, `sce_sys/`, `sce_module/`), and the `eboot.bin`
in it must be decrypted. A retail `.pkg` stays encrypted for the console it is licensed to, and the
executables inside are encrypted again, so only that console can decrypt them. Dump the installed
game from your own PS4 instead. DLC `.pkg` files are different: their content ids can be read to
unlock the DLC.

## What this port starts from

The runtime, the renderer bridge, the build scripts and the launcher all come from
[got-pc-port](https://github.com/crisprintsstuff/got-pc-port), which is itself forked from the
inFAMOUS Second Son port. They are GPL-2.0-or-later and stay that way here. The parts of that port
that are about *Ghost of Tsushima* specifically — its identity, its imports, its code offsets, its
shader hashes and its tuned settings — do not transfer, and
[`docs/PORTING-FROM-GOT.md`](docs/PORTING-FROM-GOT.md) sorts one group from the other.

Current state and the next step: [`docs/STATUS.md`](docs/STATUS.md). The per-library import counts,
measured from this dump: [`docs/IMPORTS.md`](docs/IMPORTS.md). Working in the repo (people or coding
agents): [`AGENTS.md`](AGENTS.md). Reference copy of the port this one derives from:
[`docs/UPSTREAM-GOT-README.md`](docs/UPSTREAM-GOT-README.md).

## The idea

Emulation translates the console's CPU code and re-interprets, command by command, everything it
asks of the GPU. This project moves the game onto the PC natively instead:

- **CPU: native from the start.** The game's own x86-64 code runs on the host processor. A runtime
  replaces the PS4 system libraries — memory, threads, files, saves, audio, controller — so no CPU
  emulation is involved.
- **GPU: native where it can be.** The renderer starts from shadPS4's video core, which reads the PS4
  GPU command stream and translates it to Vulkan. As with the ports before this one, the aim is to
  take the engine's rendering off that translation step by step.
- **The point:** less work per frame, so higher frame rates, lower input latency and fewer stutters,
  with room for PC options later.

None of that exists yet for Knack II. It is the direction, not a result.

## Building and running

Not available yet. The build scripts, the runtime sources and the launcher arrive from got-pc-port
with the retarget; until they are here and building, treat any build instruction as belonging to
that repository, not this one.

Everything but the Python tools will need Windows (clang-cl, Visual Studio, CMake). A Linux or cloud
agent cannot build the runtime or the renderer; it can prepare the game image and read code, and it
has to hand the build to a machine with Windows.

## License

GPL-2.0-or-later, as upstream. Knack II is a trademark of Sony Interactive Entertainment. This
project is not affiliated with Sony or SIE Japan Studio.
