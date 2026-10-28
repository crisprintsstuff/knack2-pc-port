# What carries over from the Ghost of Tsushima port

The runtime and renderer in this project are meant to start as a copy of
[got-pc-port](https://github.com/crisprintsstuff/got-pc-port), which reached its game's opening
cutscene at 30 FPS. Some of that work is about PS4 hardware and transfers to any title. Some of it is
about *Ghost of Tsushima* alone and is worthless here. This note sorts the two so nobody ports a
constant and wonders why it does not fit.

Nothing below has been done yet. This is the plan, written before the code arrives, and it should be
corrected as soon as the real build teaches something.

## Transfers, because it is the platform and not the game

- **The loader and the module linker.** SELF/ELF preparation, relocations, TLS per module and thread,
  `DTVMOD64` handling, identity-plus-version import resolution. The PS4 ABI does not change per game.
- **The kernel and library stubs, as shapes.** Memory arenas and their `MapViewOfFile3` views,
  thread and sync primitives, event queues, file I/O through a host staging buffer, RTC, offline PSN,
  save data containers, trophy plumbing, audio through SDL, pad and touchpad input. Each game calls a
  different subset with different arguments, so expect each one to need ABI checking against what
  this game's eboot actually imports.
- **The renderer bridge.** shadPS4's video core, the shim, the overrides mechanism, the way
  `build_gpu.py` maps override files over shadPS4 sources. The GPU is the same GCN hardware.
- **The generic shader recompiler fixes.** 32-lane subgroup handling for wave32 reads and shuffles,
  sharp-dword sourcing through `ReadFirstLane`, 64-bit `BitFieldInsert` lowering, the serialization
  versioning scheme. These are bugs in translating GCN to Vulkan, not facts about one game.
- **The diagnostics harness.** `-HangReport`, `-Dump`, `-DumpReg`, `ISS_WATCH`, `PROBE_EVENT`,
  `frames.csv` and its analyzer, the launcher shell that reads `game_dir.txt`.
- **The build and CI plumbing.** `build.py`, `build_gpu.py`, `build_launcher.py`, `vs_env.py`, the
  launcher workflow, `build_deps.cmd`.

## Does not transfer; redo it against this game's eboot

- **Every code address.** got-pc-port's notes are full of offsets like `+0xb60730` and
  `+0x803a00` into *Ghost of Tsushima*'s patch 2.18 eboot. They mean nothing here. Every one of them
  has to be found again in `CUSA08014`'s eboot, with its own evidence.
- **Its reverse-engineered structures.** The frame countdown, `ProxySetSync`, the job-group wait, the
  label table — those are *Ghost of Tsushima*'s engine. Knack II's engine is a different one and will
  have its own scheduling design. Do not assume a similar shape.
- **Its import inventory and stub set.** Which functions the eboot imports, and in which libraries,
  is a property of the binary. Regenerate the inventory from this game's dump; expect a different
  libc/Fios2 situation and different graphics entry points.
- **Its tuned settings and their measurements.** `ISS_UNIFIED_RANGE=0x1500000000:0x56e00000`, the
  64 KB fault window, the interrupt-merge timings, the `INT_SEL 3` workaround, the English-language
  default — each was measured against one game's behaviour. Start from the code defaults, measure,
  and write down the number for this game or leave the setting alone.
- **Its shader and pipeline caches.** Keyed by title id and by shader binary, so they cannot collide,
  and every hash in its notes (`b5500bd4`, `361a48f5`) belongs to the other game.
- **Its native hooks.** `pc-port/native/` and the eboot patches in `gpu/bridge.cpp` patch specific
  *Ghost of Tsushima* addresses; in got-pc-port they are already disabled from the inFAMOUS port.
  Carry the mechanism, not the hooks.
- **The inFAMOUS-era material.** `docs/UPSTREAM-INFAMOUS-README.md` and the Portuguese
  `pc-port/README.md` in got-pc-port describe a different game's milestones. They are history, at
  best; they are not a spec for this port.

## How the retarget should go

1. Bring the tree over without its game-specific notes, its `out/`, its caches and its handoffs.
2. Change the identity in `scripts/game_id.py` and `src/game.h`, and make the loader refuse any
   eboot whose hash does not match the dump this project is built against.
3. Prepare the dump and let the import trap stop the run. Record where it stops, which import, with
   what arguments. That is the first real fact about this port.
4. Implement in dependency order from that stop, as got-pc-port did: libc and modules first, then
   kernel services, then the first frame.
5. Only then revisit settings, and only with measurements of this game.
