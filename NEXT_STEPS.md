# Handoff notes (2026-11-25)

State at this commit, for whoever continues:

- **Regenerate the guest code after pulling.** The XenonRecomp patch changed (fall-through fix),
  so re-apply `patches/xenonrecomp-peggle2-opcodes.patch` to a fresh XenonRecomp checkout,
  rebuild it, and run the regeneration steps in the README (`gen_config.py`, `XenonRecomp.exe`,
  then rebuild `lib`). Expect 316 functions to end with "fall through into the next function".
- **Soak results (this machine, `tools/soak.ps1`).** With the fall-through fix: XMA audio off,
  0 crashes in 17 runs (3 smoke, 10 x 60 s menu, 4 x 120 s level with `PEGGLE2_AUTOPRESS=A`).
  XMA on: the sound-engine heap crash (free-list corruption, write to `0x0000000C` at
  `0x82502A60` in `0x82502A48`, thread 5, called from the allocator `0x82503008`) came back in
  2 of 9 runs. So the fall-through fix did not cure it, and it only happens with XMA decoding.
  The XMA output-ring offsets were never out of range (diagnostic in `xma.cpp`), so it is not
  the decoder writing past its ring.
- **Fixed (b5cd74f):** the audio tick snapshotted the live context list and then
  decoded into the contexts' output rings without the lock `XMAReleaseContext` takes, so a
  context released (and its buffers freed by the title's sound heap) during a tick could still
  be written. Now `XMAReleaseContext` takes the decoder lock and the tick reads the live list
  under it; the decoder also re-checks output-valid/pointer/size before every block it writes
  (`revoked` in the 10 s `[xma] ctx` log lines; it was 0 in every run so far). After the change:
  38 of 38 XMA-on runs survived: 32 x 60 s menu runs and 6 x 120 s level runs with
  `PEGGLE2_AUTOPRESS=A` (at the old rate of about 2 in 9, 38 clean runs by luck is under 0.01%).
- **Loading hitches: native zlib tried, no gain.** `runtime/zlib_native.cpp` replaces the
  title's `inflate()` (`0x8248A988`) with vcpkg's zlib on guest memory; it decodes correctly but
  one A/B in the menus gave 85 vs 110 slow frames (noise), so it is opt-in
  (`PEGGLE2_NATIVE_ZLIB=1`). The spike profile during those frames shows the render thread in
  `NtGdiDdDDIWaitForSynchronizationObjectFromCpu` (driver/fence waits), `std::vector::assign`
  and memcpy (texture copies), plus shader compiles; some spikes have very few samples, i.e. the
  whole process stalls. Next: look at render-side fence waits and per-frame texture uploads.
- **Frame pacing (30 fps solid), 2026-11-25.** Measured with the new `[frames]` lines (every
  10 s: game swap intervals, intervals between presents that showed a new frame, render-thread
  time per present; `PEGGLE2_LATE_LOG=1` logs each late frame after swap 900). Before, in a
  level, 22-34 of every 300 frames reached the screen after 50 ms instead of 33, and in the menus
  30-60 presents per 10 s repeated an old frame while as many frames were never shown. Causes
  and fixes:
  1. The guest vblank was a free-running timer re-armed from "now" (drifted with every late
     check) and not locked to the display. Now a `vblank` thread waits on the display's real
     vblank (`IDXGIOutput::WaitForVBlank`) and the GPU thread fires the guest's vblank on it
     (60 Hz displays only; otherwise, or with `PEGGLE2_VBLANK=timer`, an absolute-schedule
     timer). The GPU thread's idle wait wakes on it instead of sleeping 200 us.
  2. The render thread re-presented the last frame at 60 Hz; a new frame arriving during that
     `Present()` waited an extra vblank. It now presents only new frames.
  3. Presents were requested at `VdSwap`, which runs ahead of the GPU thread, so a present often
     drew the previous draw list. Now the GPU thread requests the present when the frame's draws
     are complete.
  4. `timeBeginPeriod(1)` for the whole run (it was only set in profiling modes).
  After: in a level 0-4 of 300 frames per 10 s are shown late (mostly 35-48 ms), with no
  repeated or skipped frames.
- **What is left:** (a) the intro and menu movies are Bink 2 at 60 fps, paced by the title on
  the audio clock (6 audio frames of 256 samples = 32 ms per step vs 33.3 ms per flip), so about
  every 0.8 s one movie frame waits an extra vblank. That is the title's own pacing (`0x82623150`
  / `0x8261D378` spin on Bink's audio-frame counter), not a decoder speed problem; all decode
  threads are idle. (b) Loading into a level: about 4 frames of 100-160 ms. The main thread is in
  the title's loading code (`0x82422D50`, `0x82490740`, `0x82427508`, `0x82404ED8`) while
  thread 11 inflates assets. The title's `memcpy` (`0x825F0AE0`, 1,083 call sites) is now a
  native `memmove` (hooks.cpp); it was the hottest single function in those frames, but one A/B
  showed only a small gain (118 vs 130 frames over 40 ms). Next ideas: replace other hot runtime
  routines (`memset`, string functions), and find what the 830-instruction `0x82404ED8` does.
- `NtReadFile`/`NtReadFileScatter`/`NtWriteFile` now signal the caller's completion event, as
  the kernel does for a request that completes synchronously (nothing in the intro used one).
- Tried and left off: XenonRecomp `cr/xer/reserved_as_local` (about 10% on loading frames) and
  `non_argument_as_local` (deadlocks at boot: threads stuck in RtlEnterCriticalSection; why is
  not understood and might point at a runtime bug worth a look).
- Debug aids: `PEGGLE2_PROFILE=spikes` (per-thread breakdown of game frames over 40 ms),
  `PEGGLE2_PROFILE=<start>,<seconds>` (whole-run profile), `PEGGLE2_AUTOPRESS=A` (A only),
  `PEGGLE2_60FPS=1`, `PEGGLE2_GPU_STATS=1`, `PEGGLE2_SHADER_SCAN=1`, `PEGGLE2_NATIVE_ZLIB=1`,
  `PEGGLE2_LATE_LOG=1`, `PEGGLE2_VBLANK=timer`,
  `tools/soak.ps1 -Runs N -Seconds S -AutoPress 1|A`.
- This machine: vcpkg is at `C:\dev\vcpkg` (the repo path has a space); configure with
  `"-DPEGGLE2_FFMPEG_ROOT=C:/dev/vcpkg/installed/x64-windows-static"`. vcpkg's static zlib is
  `libzs.lib`.
