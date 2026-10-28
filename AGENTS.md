# Agents

Instructions for any coding agent (or person) working in this repository. Mirror of the rules in
[`got-pc-port`](https://github.com/crisprintsstuff/got-pc-port/blob/main/AGENTS.md), retargeted for
this game. Read [`docs/STATUS.md`](docs/STATUS.md) first: it says where the work stops and what to do
next. Update it before you stop.

## The project

A native Windows runtime for the PS4 version of *Knack II* (`CUSA08014`; the other regional ids are
listed in the README). The game's own x86-64 code is meant to run on the host CPU, with
`pc-port/src` replacing the PS4 system libraries and `pc-port/gpu` building `iss-gpu.dll`, a Vulkan
renderer derived from shadPS4-perf.

The runtime, the renderer bridge and the scripts come from the Ghost of Tsushima port
(`got-pc-port`), which came from the inFAMOUS Second Son port. `ISS_*` names and much of the
structure are inherited from those two; when a name looks like it belongs to another game, it
probably does. Check [`docs/PORTING-FROM-GOT.md`](docs/PORTING-FROM-GOT.md) before deciding something
carries over.

- **No game files in the repo.** No eboot, modules, `.pkg`, keys, dumped shaders or assets, and none
  in commits. The user supplies a decrypted dump of their own copy. Retail `.pkg` files cannot be
  used (they stay encrypted). Do not add decryption.
- Hang reports quote small dumps of the game's code and memory from the user's own runs. They are
  for analysis only; don't commit them.

## The target build

One build, one identity, declared in two files:

- `pc-port/scripts/game_id.py` — title id, title, the expected `eboot.bin` sha256, and the modules
  that get linked when the dump has them.
- `pc-port/src/game.h` — the same title id and title for the C runtime, which keys saves, trophies
  and GPU cache paths on it.

Other regional builds are refused until someone reviews them, not silently accepted. Changing
either file is a change of target: say so in the commit subject and in `docs/STATUS.md`.

## Layout

Inherited layout once the retarget lands; the folders that do not exist yet are the work.

- `pc-port/src`: runtime (loader, libc, kernel, memory, threads, sync, offline PSN). C, Win32.
- `pc-port/gpu`: `bridge.cpp` (shadPS4 glue), `overrides/` (port versions of shadPS4 files, mapped in
  `scripts/build_gpu.py`), `shim/`.
- `pc-port/launcher`: native launcher.
- `pc-port/scripts`: `prepare.py`, `link_modules.py`, `build.py` (runtime + its tests),
  `build_gpu.py` (renderer), `build_launcher.py`, `run_game.ps1`.
- `pc-port/tests`: C/C++ tests that `build.py` runs; `libc_portable_test.c` and the Python tests also
  run on Linux.
- shadPS4 sources: `research/shadPS4-perf`, at the commit and patch the renderer builds from.

## Building and running

Everything but the Python tools needs Windows (clang-cl in `tools/llvm`, Visual Studio, CMake). A
Linux or cloud agent cannot build the runtime or the renderer: say so and ask the user to build.

## Conventions

- Branch per fix from `main`, or stacked on the branch it needs (say so in the PR). Never rewrite
  pushed history; never push to `main` directly. Open PRs when the user asks.
- Commit subject: imperative, names the area (`memory: ...`, `GPU DLL: ...`,
  `Shader recompiler: ...`). Body: what was wrong (with the evidence) and what changed.
- Comments in the code say why, with the evidence that forced the change. Match the surrounding
  style: dense C in `src/`, shadPS4 style in `gpu/overrides/`.
- A change to the shader recompiler's output bumps `ShaderBinaryVersion` in
  `gpu/overrides/vk_pipeline_serialization.cpp`, with a line in its comment.
- New diagnostics go behind an `ISS_*` environment variable and print one greppable tag.
- Add a test in `pc-port/tests` when the code can run there.
- Don't put model names in commits, comments or docs.
- Addresses in notes and reports are image offsets into **this game's** eboot (`+0x...`). Never carry
  an offset over from another game's notes, including got-pc-port's.
