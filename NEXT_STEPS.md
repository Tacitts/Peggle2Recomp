# Handoff notes (2026-11-25)

State at this commit, for whoever continues:

- **Regenerate the guest code after pulling.** The XenonRecomp patch changed (fall-through fix),
  so re-apply `patches/xenonrecomp-peggle2-opcodes.patch` to a fresh XenonRecomp checkout,
  rebuild it, and run the regeneration steps in the README (`gen_config.py`, `XenonRecomp.exe`,
  then rebuild `lib`). Expect 316 functions to end with "fall through into the next function".
- **Not yet verified:** a soak test of this exact build (10 menu runs with the intro-skip input
  plus 4 two-minute level runs with `PEGGLE2_AUTOPRESS=A`) was running when this was pushed; its
  results are not known. Earlier builds: 0 menu crashes in 20 runs after the `bdzlr` fix, and one
  run crashed in the sound engine's heap allocator (free-list corruption, write to `0x0000000C`
  at `0x82502A60`, thread 5). That crash may or may not be gone with the fall-through fix
  (the fixed switch at `0x825A63C8` is sound-engine code); run many repro runs to find out.
- **Open: loading hitches** (e.g. choosing Single Player, 100-200 ms frames). Cause: the title
  decompressing assets with its zlib 1.1.3 (`0x82500300`, thread 11) while the main thread polls
  for loads. Idea not yet tried: hook the title's zlib `inflateInit_`/`inflate`/`inflateEnd` and
  run a native zlib on a host-side z_stream mirrored from the guest one. Measure with
  `PEGGLE2_PROFILE=spikes`.
- Tried and left off: XenonRecomp `cr/xer/reserved_as_local` (about 10% on loading frames) and
  `non_argument_as_local` (deadlocks at boot: threads stuck in RtlEnterCriticalSection; why is
  not understood and might point at a runtime bug worth a look).
- Debug aids: `PEGGLE2_PROFILE=spikes` (per-thread breakdown of game frames over 40 ms),
  `PEGGLE2_PROFILE=<start>,<seconds>` (whole-run profile), `PEGGLE2_AUTOPRESS=A` (A only),
  `PEGGLE2_60FPS=1`, `PEGGLE2_GPU_STATS=1`, `PEGGLE2_SHADER_SCAN=1`.
