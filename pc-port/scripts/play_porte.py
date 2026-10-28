"""User-driven session of the port build (Claude, 2026-10-17): play_v1_guard.py with build
porte-l: the v1 native passes + global pause guard + GPU black box, plus
- readback copies on a compute-only queue (ISS_READBACK_FAMILY, default on): the main thread no
  longer waits ~2.5 ms a frame behind the rendering for particle data the GPU already wrote;
- render state key from a precomputed mask (StateWords, ~1 ms of command processor);
- async submit scan (ISS_NATIVE_PASSES_ASYNC_SCAN=1): the game submit returns at once;
- texture cache double frees logged (TEXTURE_DOUBLE_FREE) and skipped instead of an assertion;
- (porte-e) arena reads revalidated only when an arena changed, segment reads merged at close,
  draws with user clip planes recorded natively (ISS_NATIVE_PASSES_CLIP=0 turns it off).
Scripted runs: porte-d 61.8 standing / 68.9 running (before: 56.2 / 58.0); porte-e 58.6 / 69.1
(standing rises to 62-63 once the clip templates are learned).
- (porte-h) one learn request per template, not per render state (ISS_NATIVE_PASSES_TEMPLATE_GATE=0
  off): 94% of draws native; the texture garbage collector starts at 60% of the VRAM budget
  instead of 3.1 GB (it deleted ~250 textures a second still in use: the new-image spikes) and
  spares images used recently (ISS_GC_RECENT_USE=0 off). porte-h: 64.5 standing / 68.3 running.
- (porte-l, new iss-probe.exe too) the periodic 45 ms frames every ~2 s: the command processor's
  DMA copies (CopyBuffer) and the runtime's file reads (files.c) wrote the game's texture pool
  page by page, one exception per 4 KB page (~2900 a burst); both now invalidate the whole range
  first (ISS_CP_COPY_INVALIDATE=0 / ISS_RUNTIME_WRITE_INVALIDATE=0 off). porte-l: 69.9 standing.
- (porte-m) graphics queue dispatches recorded at the submission into secondaries
  (native_compute_rasterizer.inl, ISS_NATIVE_COMPUTE=0 off): 95% recorded, 10 templates; draw
  segment execution walks each texture once. porte-m: 69.8 standing / 71.3 running, CP 9.5 ms
  running (porte-l under the same counters: 65.5, CP 12.2).
- (porte-n) compute segments cut at 64 dispatches and recorded in parallel; the prepared dispatches
  are recorded into the command processor's own command buffer (from a secondary, the longhouse
  ambient lighting pass came out wrong: flat lighting, black character). Aftermath: 75 FPS, CP 5.6 ms,
  correct image (porte-m: 48 FPS, wrong image); city 67.3 / 71.3.
- (porte-o, 2026-10-18 night) shaders and startup: the shader preload reads the cache folder on 16
  threads (each file open of the game process costs ~6 ms: preload 28 s -> ~6 s); the launcher
  keeps an accumulating shader cache in %LOCALAPPDATA%/iss-port (outside OneDrive) on top of the
  shader library v2 (11 157 pipelines compiled by this build in the six scene saves, ~12 s to
  preload, ~40 s the first time on a machine), with a progress bar while it loads; compute
  segments reading data the CPU just wrote stay native (the read is uploaded as the full path
  would; Parting 1.36M native dispatches instead of 0.1M); large staging buffers in power of two
  sizes. New iss-probe.exe too (pad script R2/L2 analog).
- (porte-p, 2026-10-18 morning) a GPU write range published again in the same submission skips its
  interval set insert (ISS_PUBLISH_REPEAT_SKIP=0 off; the compute rings publish ~200k ranges a
  second in the forest, 37% repeats). Measured but left off: ISS_PREFETCH_PER_SUBMIT=1 (forest CP
  -0.6 ms, but longer readback waits in Aftermath/Parting). ISS_TUNING accepts relaxed_site=/relaxed_all=
  (readback_relaxed_sites; off: relaxing sub_3F3ED0 changed nothing measurable).
- (porte-q, 2026-10-18, frozen in base-20261018-porte-q) a graphics queue dispatch left to the
  command processor (a key given up for learning) now ends the open compute segment: before, a
  later recorded dispatch stretched the segment over it and it never ran. Off by default:
  ISS_NATIVE_ASC=1 (compute rings recorded at the DingDong; no net gain, the GDS dispatches cut the
  segments) and ISS_READBACK_REQUEST_PAGES=1 (download only the requested pages when only other
  parts of the window have GPU writes in flight).
- (porte-s, 2026-10-18 afternoon, frozen in base-20261018-porte-s; user approved the switch)
  - Native indexed indirect draws, compute rings at the DingDong (ISS_NATIVE_ASC), GDS and
    request-page readbacks on by default (porte-r).
  - The SRT walker code in chained blocks with dedup: the 32 MB block overflowed as the shader
    cache grew ("code is too big" crash).
  - Pool jobs split on indirect/auto draws too, every 48 draws.
  - Staging ring and large buffers kept instead of freed and reallocated mid-game.
  - Large upload copies on several threads: the world load-in frame 1.2 -> 0.78 s.
  City scripted against porte-r: >33 ms frames 16 -> 7, seconds below 60 FPS 2 -> 0. New
  iss-probe.exe too (ISS_PAD_SCRIPT=turn).
- (porte-u, 2026-10-18 evening, frozen in base-20261018-porte-u; user approved)
  - The window answers from its first instant: init runs off the window thread.
  - Loading screen (the game's pic1, yellow-orange bar, English).
  - Compute programs translated speculatively (ISS_SPEC_COMPUTE).
  - Large DMA copies run in parallel.
  - ISS_ASYNC_SKIP_DRAWS=1 (gow3_pc style): a graphics draw whose pipeline is still compiling is
    skipped instead of stalling the frame. Cold cache: frames >50 ms 65 -> 28. The user played
    it and kept it.
- (porte-v, 2026-10-18 evening, frozen in base-20261018-porte-v; the shader work closed)
  - A skipped draw's pipeline (or translation) moves to the front of its queue.
  - ISS_PRELOAD_BACKGROUND=1: the game starts once the cached pipelines are read and their
    compiles go on meanwhile. First frame 8.2 -> 3.5 s in the scripted city.
  - ISS_ASYNC_TRANSLATE stays off: in a cold scene it doubled the pipelines.
  Remove the two lines at the end to wait for shaders again.
- (porte-x, 2026-10-18 night; user approved, built without the draw distance research)
  - Full screen passes are never skipped (the yellow-green screen when the world spawned).
  - BB_FPS_LIMIT works (launcher "FPS limit").
  - ISS_ASYNC_SKIP_DRAWS back to 0: skipped draws made trees blink on LOD swaps.
- (porte-x-save, 2026-10-18 night; the current source, also the release's iss-probe/iss-gpu)
  - PS4 style save dialog over the game (gpu/save_dialog_ui.cpp), controller, keyboard, mouse.
  - DLC labels (ISS_ADDCONT_LABELS), window title "inFAMOUS Second Son".
  The release (scripts/build_release.py) is kept apart; this script is the source/debug build.
- (porte-aot3, 2026-10-19; user approved) shaders ahead of time + night fixes:
  - ISS_AOT_COMPUTE=auto + ISS_AOT_WAIT=1: the game's compute shaders compiled before playing
    (first run ~1.5 min "Preparing shaders", then ~1 s); the shader index is made by the DLL.
  - ISS_NATIVE_ASC_ASYNC=1: compute rings recorded off the game thread (+4% uncapped).
  - The ~350 ms hitch every session (blackbox self test) is gone. Previous build: porte-x-save.
- (porte-vsync1, 2026-10-19; user asked to play it) the 75 Hz stutter fixes: present acquire fence
  (the 26 ms vsync misses), residency memory pool, CP DMA batching, merged barriers. City at 75 Hz:
  frames >25 ms 17 -> 1.4 a minute. Previous build: porte-aot3."""
import play_v1_guard as play

play.BUILD = play.OUT / 'experimental/20261016-native-passes-v1/base-20261019-porte-vsync1'
play.NATIVE['ISS_NATIVE_PASSES_ASYNC_SCAN'] = '1'
play.NATIVE['ISS_ASYNC_SKIP_DRAWS'] = '0'
play.NATIVE['ISS_PRELOAD_BACKGROUND'] = '1'
play.NATIVE['ISS_AOT_COMPUTE'] = 'auto'
play.NATIVE['ISS_AOT_WAIT'] = '1'
play.NATIVE['ISS_NATIVE_ASC_ASYNC'] = '1'
play.NATIVE['ISS_GPU_BLACKBOX_TEST_S'] = '0'

if __name__ == '__main__':
    play.main()
