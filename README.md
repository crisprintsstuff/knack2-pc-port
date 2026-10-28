# Knack II PC port (CUSA08014)

A native Windows runtime for the PS4 version of *Knack II*, built the same way as
[crisprintsstuff/got-pc-port](https://github.com/crisprintsstuff/got-pc-port): the game's own
x86-64 code runs directly on the host CPU, a custom runtime replaces the PS4 system libraries, and
graphics go through a Vulkan renderer derived from shadPS4.

**Status: nothing has run yet.** This repository is the starting point, not a working port. No build
exists, no dump has been prepared and the game has not been executed. What exists today is the
identity of the target, the rules for working here, and a record of what carries over from the Ghost
of Tsushima port and what has to be redone from scratch.

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

The decrypted `eboot.bin` hash for this build is not recorded yet. It has to be read off a real dump,
never guessed and never borrowed from another title.

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

Current state, open work and the next step: [`docs/STATUS.md`](docs/STATUS.md). Working in the repo
(people or coding agents): [`AGENTS.md`](AGENTS.md). Reference copy of the port this one derives
from: [`docs/UPSTREAM-GOT-README.md`](docs/UPSTREAM-GOT-README.md).

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
