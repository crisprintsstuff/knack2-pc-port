# Status

Last updated 2026-10-28. Update this file whenever a run, a branch or a finding changes what the next
step is. Newest state first; keep the history short.

## Where the work stops

**Nothing has been built or run.** This repository starts on 2026-10-28 with its identity, its rules
and its notes, and no code, no build and no dump prepared for it. The runtime and renderer are still
in [got-pc-port](https://github.com/crisprintsstuff/got-pc-port); bringing them over is the first
task.

What is established:

- The target is *Knack II*, PS4, title id `CUSA08014` (US). The other regional ids are
  `CUSA07670` (Europe/Australia), `CUSA08095` (Asia) and `CUSA04774` (Japan), each a separate eboot.
- The architecture to start from is got-pc-port's: native CPU execution with a PS4 service layer,
  shadPS4-derived Vulkan renderer. See [`docs/PORTING-FROM-GOT.md`](PORTING-FROM-GOT.md) for what
  carries over and what must be redone.
- The decrypted `eboot.bin` sha256 for `CUSA08014` is not recorded. It has to come from a real dump.

## Next step

1. Bring the got-pc-port tree over without its game-specific notes, its `out/` and its caches.
2. Retarget `pc-port/scripts/game_id.py` and `pc-port/src/game.h` to `CUSA08014`, with the eboot hash
   read from the user's dump, and make the loader refuse anything else.
3. Prepare the dump and run to the first unimplemented import. Record the stop, the import, and the
   arguments in this file.

## Known unknowns

Everything about how this game boots. Its import inventory, its libc and Fios2 situation, its
graphics entry points, its memory layout, and whether its engine resembles the ones the previous
ports dealt with. Nothing here should be assumed from another title.

## Diagnostics

Inherited from got-pc-port once the code is in: hang reports, dumps, `ISS_WATCH`, `PROBE_EVENT`,
`frames.csv`. None of it exists in this repository yet, and none of its recorded offsets or hashes
apply to this game.
