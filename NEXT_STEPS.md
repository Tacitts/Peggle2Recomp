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
- Tried and left off: XenonRecomp `cr/xer/reserved_as_local` (about 10% on loading frames) and
  `non_argument_as_local` (deadlocks at boot: threads stuck in RtlEnterCriticalSection; why is
  not understood and might point at a runtime bug worth a look).
- Debug aids: `PEGGLE2_PROFILE=spikes` (per-thread breakdown of game frames over 40 ms),
  `PEGGLE2_PROFILE=<start>,<seconds>` (whole-run profile), `PEGGLE2_AUTOPRESS=A` (A only),
  `PEGGLE2_60FPS=1`, `PEGGLE2_GPU_STATS=1`, `PEGGLE2_SHADER_SCAN=1`, `PEGGLE2_NATIVE_ZLIB=1`,
  `tools/soak.ps1 -Runs N -Seconds S -AutoPress 1|A`.
- This machine: vcpkg is at `C:\dev\vcpkg` (the repo path has a space); configure with
  `"-DPEGGLE2_FFMPEG_ROOT=C:/dev/vcpkg/installed/x64-windows-static"`. vcpkg's static zlib is
  `libzs.lib`.
