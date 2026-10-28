# Status

Last updated 2026-10-28. Update this file whenever a run, a branch or a finding changes what the next
step is. Newest state first; keep the history short.

## Where the work stops

**Nothing has been built or run.** On 2026-10-28 the decrypted US dump was inspected and its identity
recorded; the runtime, the renderer, the launcher and the scripts are still in
[got-pc-port](https://github.com/crisprintsstuff/got-pc-port). Bringing them over is the next task.

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

1. Bring the got-pc-port tree over without its game-specific notes, its `out/` and its caches.
2. Teach the loader the two-stage boot: link `eboot.bin`, let it load `/app0/game.elf`, and run the
   game's entry at `+0x80` instead of the stub's `+0x4fa0`. This has no equivalent in got-pc-port and
   is the first real piece of new code.
3. Run to the first unimplemented import and record it here with its library and arguments.

## Known unknowns

How the game behaves once it starts. Whether `libSceJobManager` can be satisfied by the runtime or has
to be linked from the dump; whether `libSceJson2` and the font libraries need work before the first
frame; the GPU entry points it uses and in what order; the memory layout it expects. None of this
should be assumed from another title.

## Diagnostics

Inherited from got-pc-port once the code is in: hang reports, dumps, `ISS_WATCH`, `PROBE_EVENT`,
`frames.csv`. None of it exists in this repository yet, and none of its recorded offsets or hashes
apply to this game.
