# Peggle2Recomp

An unofficial native Windows port of the Xbox 360 version of Peggle 2 (XBLA, title
`58411436`), built by statically recompiling the game's PowerPC code into C++ with
[XenonRecomp](https://github.com/hedge-dev/XenonRecomp). There is no CPU emulation: the
game's code runs as a native x86-64 program on a reimplemented Xbox 360 kernel and system
layer (memory, threads, files, profiles, networking, audio driver).

**Status:** work in progress. The game boots, runs its full startup and keeps its frame
loop going at ~30 fps, but nothing is drawn yet; the D3D12 renderer is next.

This repository contains **no game code or assets**. You need your own legally obtained
copy of Peggle 2 for Xbox 360; the tools extract and recompile it locally. `game/`, `ppc/`
and `build/` hold material derived from your copy and are git-ignored: never upload them.

Not affiliated with or endorsed by PopCap Games or Electronic Arts.

## Setup

Requirements: Windows 10 1803+, Visual Studio 2022 Build Tools (C++), LLVM/Clang 18+,
CMake, Ninja, Python 3.12, Git.

```powershell
git clone --recursive https://github.com/hedge-dev/XenonRecomp.git third_party/XenonRecomp
git clone --recursive https://github.com/hedge-dev/XenosRecomp.git third_party/XenosRecomp
git -C third_party/XenonRecomp apply ../../patches/xenonrecomp-peggle2-opcodes.patch
git -C third_party/XenosRecomp apply ../../patches/xenosrecomp-peggle2.patch
```

Optional, for music and sound effects (XMA audio): FFmpeg's codec library from vcpkg, cloned
next to this repository (`../vcpkg`, e.g. `C:\dev\vcpkg` for a clone at `C:\dev\Peggle2Recomp`).
The first build takes 20-40 minutes. Without it everything builds and runs; XMA sounds are silent.

```powershell
git clone https://github.com/microsoft/vcpkg ..\vcpkg
..\vcpkg\bootstrap-vcpkg.bat -disableMetrics
..\vcpkg\vcpkg install "ffmpeg[core,avcodec]:x64-windows-static"
```

Then follow "Reproduce" below, pointing `stfs_extract.py` at your own Peggle 2 package file
(the `58411436\000D0000\...` file from your console/storage device).

Useful analysis scripts used during bring-up live in `tools/analysis/` (call graphs, store
finders, `.pdata` lookups, shader-container scanning); they take the decrypted image
written by `xexdump --dump-image`.

## Status

| Phase | State |
|---|---|
| 1a. Extract package (STFS) | Done. All 225,176 blocks SHA-1 verified. |
| 1b. Decrypt/parse `Default.xex` | Done. Base `0x82000000`, entry `0x823186D0`, `.text` 7.3 MB, 1.95M PPC instructions. |
| 1c. Jump tables | Done. 197/197 bounds-checked tables resolved; 0 switch errors in the recompile. |
| 1d. Function boundaries | Done. 25,066 `.pdata` functions, 17 leaf functions with tables bounded explicitly, exception-handler records skipped. |
| 1e. PPC → C++ | Done. 33,820 functions in 134 files in `ppc/`. 0 unrecognized instructions, 0 error markers. (An earlier count of 34,268 included stale files from previous runs; always regenerate into an empty `ppc/`.) |
| 2. Missing opcodes in XenonRecomp | Done. 48 opcodes added, plus three upstream fixes (see below). `lib/` compiles all generated code into `Peggle2Recomp.lib` (53 MB) with 0 errors and 0 warnings. Instruction semantics **reviewed**: the C++ emitted for every added opcode was checked against the PowerPC/VMX definitions (lane order under byte-reversed vector storage, saturation, operand order of the packs, sign extension, update forms, carries, branch conditions); no errors found. Not executed as unit tests: Xenia's PPC tests need a PowerPC assembler (the Windows LLVM build has no PowerPC target). |
| 3. Shaders (XenosRecomp) | Done for everything seen so far. Shaders are captured as raw microcode from the GPU command stream (`IM_LOAD` / `IM_LOAD_IMMEDIATE`) and translated at runtime: **9/9** (5 vertex, 4 pixel) cover the title, menus and the first level, and all compile. See 4h. |
| 4a. Runtime skeleton | Done. `build/runtime/Peggle2.exe` boots the recompiled code natively: CRT init, XAM module lookups, file I/O, and into D3D/GPU initialisation. |
| 4b. Threads + sync objects | Done. Real host threads per `ExCreateThread` (6 guest threads run: main, Wwise `AK::IOThread`/`AK::BankManager`, audio, 2 D3D workers), events, semaphores, mutants, timers, waits, spinlocks, object manager. |
| 4c. Audio driver | Done. Render-driver callback ticks every 5.33 ms on a guest audio thread, XMA contexts allocated, notification listeners implemented; Wwise now initialises fully (`AK::EventManager` thread starts). Frames from all clients are summed per tick, downmixed from 6 channels to stereo and played through WASAPI (`audio.cpp`); the log reports the peak level every 10 s. |
| 4d. Profiles / sign-in / network | Done. One signed-in offline profile ("Player", XUID `0xE000000000000001`), profile settings read/write (persisted to `userdata/profile/settings.bin`), license mask, offline network (no link, no address). |
| 4e. App object init | Done. Root cause was networking: RakNet (`RakPeer::Startup`) failed, the app object was destroyed right after construction, and later code used the dangling pointer. Fixed with a MAC address in `XNADDR` and a real Winsock-backed socket layer (VDP mapped to UDP). RakNet now starts on 127.0.0.1:1000. |
| 4f. Intro / main loop | Done. The game reaches its frame loop and presents 2 frames (`VdSwap`), 19 guest threads, no crash. Fixed on the way: voice engine (XHV) profile query for user `0xFF`, achievement/stats enumerators (an unwritten buffer size caused a 1 GB `new` and `std::bad_alloc`). |
| 4g. GPU command processor | Done (no rendering). The frame loop runs continuously: ~30 presents/s, ~300k PM4 packets and ~16k fences per minute, 60 Hz vblank. See "GPU notes". |
| 4h. Renderer | **Working for the title, menus and the first level.** Approach: translate the GPU command stream. **Shaders** (`xenos_shader.cpp`): microcode from `IM_LOAD*` packets is wrapped in a synthetic container (no constant table; one vertex element per vfetch as `TEXCOORDn`; interpolators 0-15), translated by XenosRecomp in-process and compiled by `dxcompiler.dll` at runtime; HLSL and microcode are saved to `build/runtime/microcode/`. Constants come from the raw register file, texture fetch slot N is descriptor N. (Earlier attempts captured shader *containers* from memory: only the 3 Bink movie shaders go through the creation hook, and the frame-300/1200 memory scan found headers whose microcode had been overwritten, which is why "19 of 28 failed"; the scan now writes to `shaders_scan/`.) **Draws** (`gpu.cpp` records, `draw.cpp` executes): auto-indexed triangle lists/strips, fans and quads via generated indices, rectangle lists expanded on the CPU; vertex streams byte-swapped per fetch constant; textures decoded on the CPU (8, 8_8, 565, 8888, DXT1/3/5) with the fetch swizzle; samplers, blending, colour mask, scissor, simple alpha test. **EDRAM**: each surface (colour base + pitch) is a host render target at native-resolution scale; `RB_MODECONTROL` mode 6 draws are resolves (scissor rectangle copied to a host texture keyed by `RB_COPY_DEST_BASE`, sampled by later fetches with the resolve's R/B swap); the texture `VdSwap` names is presented. The first level draws its playfield to a 960-wide surface and resolves it, which now works. **Not done**: depth (mode 5 draws are skipped), indexed draws, MSAA, other surface formats, resolves to memory for CPU readback. Debugging: `PEGGLE2_DUMP_FRAME=<n>` logs every draw's state for host frame n; `PEGGLE2_PREVIEW=1` shows the old texture preview instead of draws. |
| 4i. Saves / content | Done (`content.cpp`). Device selector picks the one hard drive (device 1), `XamContentGetDeviceData`, `XamContentCreateEx/Close/Delete` with packages in `userdata/content/<type>/<file name>/` mounted at their root name (`savegame:`). A missing package returns `X_ERROR_PATH_NOT_FOUND`, and overlapped extended errors are `HRESULT_FROM_WIN32` (as in Xenia): with `FILE_NOT_FOUND` the game retried opening `PROFILE` forever. The game now creates and reloads its `PROFILE` save and reaches the main menu. |
| 4j. Input | Done (`input.cpp`). Guest controller 1 merges the first host XInput controller (same `XINPUT_GAMEPAD` layout), the keyboard and `PEGGLE2_AUTOPRESS`. Keys come from the game window's own `WM_KEYDOWN/UP` (render.cpp), so they follow focus and work with injected input; a press is latched until the next poll so taps shorter than a frame still count. Keys: arrows = D-pad, Space/Z = A, Esc/X = B, C = X, V = Y, Enter/P = Start, Tab/Backspace = Back, Q/E = LB/RB, Shift/Ctrl = LT/RT, WASD = left stick, IJKL = right stick. Verified: key presses reach the game as buttons and move it from the title into the main menu. Reaching a level by hand needs to see the menus, i.e. real draws (4h). |
| 4l. Display | Done (`render.cpp`). Resizable window; F11 or Alt+Enter toggles borderless fullscreen (these keys are not passed to the game); F12 saves a screenshot to `build/runtime/screenshots/` (`PEGGLE2_SCREENSHOT_FRAME=n,m,...` captures given frames); the 1280x720 frame is letterboxed and rendered at the window's resolution. |
| 4m. Frame rate | **30 fps is the game's own target**, as on the console: every flip request is `0x205` (swap interval 2, 5% tear threshold), and the title's vblank handler (`0x8261EF88`) flips every second 60 Hz vblank. A frame that misses its 33 ms slot waits for the next vblank (20 fps for that frame). Profiling (`PEGGLE2_PROFILE=<start s>,<seconds>`: samples every thread about once a millisecond and logs each thread's time per function; threads are named) showed the main thread waiting on that flip, the GPU thread mostly idle and no CPU-bound thread. The logic itself runs at a fixed 60 updates/s: the frame step (`0x8242F8A8`) runs two updates per drawn frame. **Optional 60 fps:** `PEGGLE2_60FPS=1` skips the second update of each frame step and flips at interval 1, so the game draws every update: measured 59-60 fps at 59.6 updates/s (30 fps at 59.7 updates/s without it), same game speed. Off by default: a frame slower than 16.7 ms then slows the game down, as on a 60 fps console game. Checked with timed screenshot sequences in a level: the pause-menu slide (0.2 s per step) and the badge animation (1.0 s from appearing to full size) take the same time in both modes (`PEGGLE2_AUTOPRESS=A` presses only A, so it plays without pausing). Dips fixed on the way: the movie-texture retry slept 0.5 ms, which on Windows lasts a timer tick (up to 15.6 ms) and made intro frames miss their vblank (22 fps when the unicorn hits the logo); it now spins. The old shader memory scan at frames 300/1200 (~0.3 s stall) now runs only with `PEGGLE2_SHADER_SCAN=1`. |
| 4k. XMA decoding | Done, confirmed by ear in the menus and a level (`xma.cpp`, FFmpeg's xma2 decoder). Each audio tick, before the render-driver clients mix, every live context (Xenia's documented `XMA_CONTEXT_DATA` layout) with a valid output ring and input buffer is decoded: 2 KB XMA2 packets to 16-bit big-endian PCM in 256-byte blocks, write offset advanced, finished input buffers invalidated and the other buffer taken; a read offset moved by the title restarts the decoder there. The title reads the output: its read offset follows ours. In the menus, ~1,200 packets/min decode (music: 48 kHz stereo, streamed in ~8-packet buffers, decoded peak ~0.4); a test tone (`PEGGLE2_XMA_TEST=1`) confirms decoded audio reaches the mix, the menu mix peaks at ~0.04, which sounds right in practice. Needs FFmpeg from vcpkg (see Setup). The log reports per-context packets and decoded peak every 10 s. Hardware loops (loop count 255 with loop start/end) are frame-accurate: an XMA2 frame map per buffer, passes cut to the exact sample count, the decoder drained at the loop end; the reported read offset is the next frame to decode (like Xenia). The intermittent main-menu crash in the sound engine (about 1 run in 3 when the intro was skipped) was a recompiler bug in the game's `memset`, not an audio bug: see the `bdzlr` fix below. 20 runs after the fix: 0 of those crashes (9 of 20 before). **Known bug:** one of the 20 crashed in the sound engine's heap allocator (free-list corruption, write to `0x0000000C` at `0x82502A60`, thread 5), cause not found yet. |

## Layout

```
recomp/
  game/                    extracted package (Default.xex, main.rsb 875 MB, icons, ArcadeInfo.xml)
  config/
    peggle2.base.toml      hand-maintained recompiler config (edit this)
    peggle2.toml           GENERATED = base + peggle2_functions.toml
    peggle2_switch_tables.toml   GENERATED jump tables
    peggle2_functions.toml       GENERATED leaf-function bounds
  ppc/                     GENERATED C++ (ppc_recomp.*.cpp, ppc_config.h, ppc_func_mapping.cpp)
  tools/
    stfs_extract.py        STFS (CON/LIVE/PIRS) extractor with hash verification
    xexdump/               section dump, save/restore helper finder, bctr survey, --disasm
    jumptables/            jump-table finder (concrete interpretation) + leaf bounds
    gen_config.py          merges generated function bounds into peggle2.toml
    build.cmd              MSVC env + clang-cl + Ninja CMake build helper
  third_party/             XenonRecomp, XenosRecomp (git clones)
  build/                   tool builds, Default.image.bin (decrypted image), recomp.log
```

## Reproduce

```powershell
$py = "$env:LOCALAPPDATA\Programs\Python\Python312\python.exe"
& $py -I tools\stfs_extract.py "<path to your Peggle 2 package file>" game --verify
cmd /c "tools\build.cmd tools build\tools"
.\build\tools\xexdump.exe game\Default.xex --dump-image build\Default.image.bin
.\build\tools\xexdump.exe game\Default.xex --imports > build\imports.txt
cmd /c "tools\build.cmd third_party\XenonRecomp third_party\XenonRecomp\build"
cmd /c "tools\build.cmd tools build\tools"
.\build\tools\jumptables.exe game\Default.xex config\peggle2_switch_tables.toml config\peggle2_functions.toml
& $py -I tools\gen_config.py .
.\third_party\XenonRecomp\build\XenonRecomp\XenonRecomp.exe config\peggle2.toml third_party\XenonRecomp\XenonUtils\ppc_context.h > build\recomp.log
```

## Findings specific to this binary

- **Jump tables.** XenonAnalyse finds none: this compiler inserts `nop`s and reorders
  `lis/rlwinm/addi`. `tools/jumptables` instead finds `cmplwi crN,rI,N` + `bgt/ble crN`
  ahead of each `mtctr/bctr` and interprets the code between them for every index.
  Kinds seen: absolute word tables, `lhzx` short-offset, `lbzx` byte-offset, and
  `lbzx`+shift computed tables. One table has no bounds check at all (0x821C7328, 28 cases):
  the table sits right after the `bctr` and its size is where the first case begins; the
  detector handles that shape too. Missing it crashed the game at the end of the first
  level (indirect call to 0x821C73B0, not a function start). The other `bctr`s (~594) are
  virtual/indirect calls.
- **Exception records.** 8-byte `{handler, data}` pairs precede functions flagged in
  `.pdata`: handler `0x825F2878` (2,137 functions) and `0x8288447C` (74 functions).
  Without skipping them, XenonRecomp's linear sweep analyses them as code and emits
  large overlapping bogus functions.
- **Save/restore helpers** (`__savegprlr_14` etc.) are at `0x825F0A30`–`0x825F221C`.
- **Third-party code** in its own sections: Bink video (`BINK`) and RAD (`RADCODE`).
  Peggle 2 shows Bink movies, so the runtime needs a Bink decoder or a replacement.
- `setjmp`/`longjmp` are not located yet (`TODO` in `peggle2.base.toml`).

## XenonRecomp changes (`patches/xenonrecomp-peggle2-opcodes.patch`)

Apply to a fresh clone with `git -C third_party\XenonRecomp apply ..\..\patches\xenonrecomp-peggle2-opcodes.patch`.

- **48 new opcodes**, in one marked block at the top of the instruction switch in
  `XenonRecomp/recompiler.cpp`: update-form loads/stores, `addc/addme/subfze/eqv/mulhdu`,
  `frsqrte`, `bso/bns/bdzf/bdnzt`, and halfword/word VMX integer ops. Vector registers are
  stored fully byte-reversed, so lane-wise ops map lane i to lane i; `vpkswss` swaps operands
  like upstream's `vpkshus`.
- **Fix: CTR branches to other functions.** Upstream `bdz/bdnz/bdnzf` always emitted
  `goto loc_X`, which fails to compile when X is another function (a tail call, seen at
  `0x825A63C8`). They now call the function and return, like the other conditional branches.
- **Fix: `bdnzf` condition bit.** Upstream always tested `eq`; it now tests the bit encoded
  in the instruction.
- **Fix: `bdzlr`/`bdnzlr` ended the function.** XenonAnalyse treated any `bclr` with BO bit
  0x10 (ignore the condition) as an unconditional return; it is unconditional only with BO
  0x04 (leave CTR alone) too. The game's `memset` (`0x825F10B0`) ends in `stb; bdzlr; stb;
  bdzlr; stb; blr`, so its last two byte stores became separate functions that were never
  reached: every `memset` of 4n+2 or 4n+3 bytes left the last 1-2 bytes unwritten. One
  victim: the sound engine's random container clears its two 1-byte flag arrays with one
  2-byte `memset`, the second kept old heap data, the item count and the flags disagreed,
  and the pick walked past the item list (null entry at `0x825A5D38`). These are the only
  two `bdzlr` in the binary.

Build the guest-code library:

```powershell
$env:BUILD_ARGS = '-- -k 0'; cmd /c "tools\build.cmd lib build\lib"
```

### Opcodes that were missing (for reference)

Sites per opcode, before the patch:

`bdzf 731, vsrah 477, stfsu 456, vminsw 296, vslh 295, lfsu 290, sthu 179, vpkswss128 115,
lhzu 114, vspltish 97, lfsux 78, eqv 73, vsubuwm 72, vavguh 67, lhzux 57, lwzux 43, dcbst 41,
subfze 29, bso 29, stfdu 22, addme 19, vsrh 18, stfsux 17, addc 15, lfdu 15, lbzux 13,
vmaxuh 12, vminuh 12, stbux 10, sthux 8, vaddsws 6, vsububm 6, vcmpgtsb 6, vcmpgtsw 6, bns 5,
lvehx 4, ldux 4, vsel128 3, vpkswss 3, subfze. 3, lhau 3, bdnzt 2, mulhdu 2, lwaux 2,
vsubshs 1, frsqrte 1, lfdux 1, stdux 1`

## Runtime (`runtime/`)

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\run.ps1 -Seconds 30
```

`run.ps1` regenerates the import stubs, builds `build/runtime/Peggle2.exe`, runs it with
`game/` for a limited time, and prints the end of `build/runtime/peggle2_runtime.log`.

| File | Contents |
|---|---|
| `runtime.h` | Guest memory helpers (big-endian loads/stores), allocators, `IMPORT(Name)` macro, handle table |
| `main.cpp` | Boot: 4 GB reservation, XEX load, indirect-call table, GPU register window, crash reporter (maps host RIP back to the guest function), `_xstart` |
| `memory.cpp` | Address space and bump allocators (frees ignored) |
| `kernel.cpp` | Thread setup (KPCR/KTHREAD), import slots + kernel variables, memory, critical sections, TLS, strings, XEX header queries, module handles / `XexGetProcedureAddress` |
| `fs.cpp` | Handle table, `Nt*File` on the host file system |
| `threads.cpp` | Threads, events, semaphores, mutants, timers, waits, spinlocks, `Ob*`, DPCs, thread-name exceptions. All waitable state lives in the guest `DISPATCHER_HEADER`, guarded by one global lock + condition variable. |
| `gpu.cpp` | Video driver (`Vd*`) imports, the PM4 command processor, shader microcode capture (`IM_LOAD*`) and per-draw records for the renderer |
| `audio.cpp` | XAudio render driver (client callback every 256-sample frame, called with a pointer to its argument), host output through WASAPI, XMA context allocation, voice/ducker queries |
| `xma.cpp` | XMA decoder emulation on FFmpeg (optional at build time) |
| `xmsg.cpp` | `XMsg*` requests to system apps; the dashboard music player (0xFA) reports idle with the title in control of playback |
| `xam.cpp` | Notification listeners (sign-in notification posted to system listeners), signed-in profile (`XamUserGetSigninInfo/State/XUID/Name`, privileges), profile settings read/write with persistence, license mask, `CompleteOverlapped()` for async XAM calls |
| `net.cpp` | No Xbox Live / no address, but a stable MAC in `XNADDR` (RakNet's GUID) and real sockets on host Winsock (`IPPROTO_VDP` 254 mapped to UDP). Most `NetDll_` calls take a leading `caller` argument; `inet_addr`, `WSACreateEvent`, `WSACloseEvent` do not. |
| `content.cpp` | Storage device selector and device data (one hard drive), content packages (`XamContentCreateEx/Close/Delete`) as host directories mounted at their root name |
| `input.cpp` | `XamInputGetState/SetState/GetCapabilities`: controller 1 = host XInput controller + keyboard (+ `PEGGLE2_AUTOPRESS`), 2-4 not connected |
| `hooks.cpp` | `TRACE_FUNC(addr, label)` overrides any recompiled function to log arguments and result (XenonRecomp weak-alias mechanism). Also mirrors the game's own formatted messages (`0x82453B78`) into the log as `[game]` lines, and traces `RakPeer::Startup`. |
| `generated/` | From `tools/gen_imports.py`: import slot tables and logging stubs for every import without an `IMPORT()` |

Guest memory map: image `0x82000000`, virtual heap `0x40000000-0x7BFFFFFF`, runtime
structures `0x7C000000-0x7EFFFFFF`, GPU registers `0x7FC80000-0x7FCFFFFF`, physical heap
`0xA0000000-0xBFFFFFFF`, indirect-call table after the image (`PPC_LOOKUP_FUNC`).

File paths: `game:\`, `d:\`, `\Device\Cdrom0\` map to `game/` (read-only); other drives
(`cache:\` etc.) map to `build/runtime/userdata/<drive>/`.

Imports: 212 functions (57 implemented, 155 logging stubs) and 12 kernel variables.
KPCR/KTHREAD/LDR offsets follow Xenia's published layouts and are marked in `kernel.cpp`.

### Debugging aids

- **Exact guest instructions in crash reports.** The patched XenonRecomp writes the guest
  address in every instruction comment (`// 82145888 lwz r10,0(r3)`), the guest library is
  built with line tables (`/Z7 -gline-tables-only`) and the exe with a PDB; the crash
  reporter maps each frame's host address to its source line via DbgHelp and prints the
  guest instruction, e.g. `#0  guest 82145888 lwz r10,0(r3)  (in function 0x821457C0)`.
- **C++ exceptions** thrown by the game (`RtlRaiseException` code `0xE06D7363`) are decoded:
  type names, `what()` and the throw site. They are not unwound (XenonRecomp has no
  exception support), so a throw is effectively fatal; treat it as a symptom.
- Crash reports include the guest call chain (host stack unwound with `RtlVirtualUnwind`,
  each frame mapped back to its guest function), the faulting guest address, the guest
  thread and the last import it called.
- Every guest stack has a decommitted 64 KB guard page below it.
- `[mem]` lines log every virtual/physical allocation, commit and free.
- `WatchGuestWrite(addr)` arms an x64 hardware write watchpoint (DR0) on a guest address
  and logs every write with the guest call chain (this found the RakNet failure path).
- Caveat: a hooked function's original body is attributed in call chains to the guest
  function just before it (the function table points at the hook).
- Threads that are not blocked in a kernel wait get a **sampled stack** in the `[watch]`
  output (suspended briefly, unwound without locks, symbolised after resuming).
- The jump-table detector also handles tables bounded by a mask instead of a compare
  (`clrlwi`/`andi.`) and tables indexed through `index + c` (203 tables).
- `[watch]` lines every 10 s list each guest thread's last import and, if it is blocked, the
  object it waits on (type and signal state). Use this for hangs.
- Every function-table slot without a recompiled function holds a trap that reports the
  guest target of the indirect call (taken from CTR). This found two jump tables the
  detector had missed (`ble in_range; b default; ...` at `0x827BE3E8`, and a bounds check 25
  instructions before the table at `0x8248AA3C`); both shapes are handled now (199 tables).

### Runtime bugs found so far (worth knowing for any XenonRecomp runtime)

- `PPCFPSCRRegister::csr` must be seeded from the host (`ctx.fpscr.loadFromHost()`); a
  zero-initialised context writes `MXCSR = 0` on the first FPSCR change, unmasking every
  SSE exception (process dies with `0xC000008F`).
- `NtAllocateVirtualMemory` must write back the page-rounded base and size on commit, and
  honour 64 KB pages for `MEM_LARGE_PAGES` (`0x20000000`); the title's heap depends on both.
- `MEM_DECOMMIT` must really decommit (re-committed pages must read as zero).

### What the game does on boot (from the log)

1. CRT init: critical sections, TLS, heap via `NtAllocateVirtualMemory`/`XamAlloc`.
2. Loads `xam.xex` and asks for newer optional exports (`XamPartyGetUserList`,
   `XamShowPartyUI`, ...); "not found" is the correct answer and the game handles it.
3. Creates events, a worker thread, network startup.
4. Looks for optional `properties\partner.xml` (absent on retail; fine).
5. Starts Wwise threads (`AK::IOThread`, `AK::BankManager`) and the audio render thread;
   registers an XAudio render-driver client.
6. GPU init: `VdInitializeEngines`, ring buffer, interrupt callback, two D3D worker threads,
   display queries, several 3.6 MB physical allocations (render targets / pools).
7. Wwise: memory pools, command queue (needs a notification listener for area `0x20`),
   `AK::EventManager` thread. Bink starts its async thread (`BinkAsy3`). 10 guest threads.
8. Back in top-level init (`0x82195DD0`): sign-in info, XUID, reads 3 profile settings
   (empty profile), sets rich-presence contexts (`XMsgStartIORequest(0xFB, 0x000B0006)`),
   network startup, polls `XNetGetTitleXnAddr` until it reports no network.
9. Builds the app object; its constructor starts RakNet (`RakPeer::Startup`, vtable
   `0x820E8260`): GUID from the MAC address, UDP/VDP socket bound to port 1000.
   (Earlier failures: Reason=10 with a zero MAC, Reason=5 when sockets were refused.)
10. Sets up audio (`0x82142F70`): XAudio2 mastering voice, then the voice engine (XHV,
    `0x8230A968`), which reads dashboard voice settings for user `0xFF`.
11. Opens `d:\main.rsb`, polls controllers, looks for `subtitles\wordmark_bjorn_sound.json`,
    starts Bink threads for the intro movie, creates an achievement enumerator.
12. Enters the frame loop: `VdSwap` x2, then waits on the GPU (see 4g).

### Memory model

Physical RAM is one 512 MB shared section mapped three times, at `0xA0000000` (64 KB pages),
`0xC0000000` (16 MB pages) and `0xE0000000` (4 KB pages), as on the console: the game reads
GPU data through the `0xC0000000` alias. Built with placeholder reservations
(`VirtualAlloc2` / `MapViewOfFile3`, Windows 10 1803+). GPU (physical) address P is
`0xA0000000 | P` for the CPU.

### GPU notes (`gpu.cpp`)

- A GPU thread consumes the ring buffer from `CP_RB_WPTR` (MMIO register `0x1C5`) and writes
  the read pointer back. It executes the synchronisation packets (type-0/1 register writes,
  scratch registers mirrored to `SCRATCH_ADDR`, `INDIRECT_BUFFER`, `WAIT_REG_MEM`, `REG_RMW`,
  `COND_WRITE`, `MEM_WRITE`, `EVENT_WRITE_SHD`, `INTERRUPT`) and skips draws/state.
- **D3D's CPU callback handshake** uses the scratch registers: the GPU sets scratch 1 = 1,
  scratch 4/5 = callback + argument, scratch 0 = command, raises an interrupt on CPU 2, and
  waits for scratch 0 and then scratch 1 to return to 0.
- The graphics interrupt handler (`0x82622E88`) needs: the target CPU number in
  `KPCR+0x10C`, and the display interrupt status register `0x1951` reading 1, or it never
  runs its vblank processing (frame pacing, callback thread wake-up) and everything stalls.
  `0x194C` (V counter) and `0x1961` (viewport size) are published as well.
- `VdSwap` fills the 64 dwords D3D reserves in the ring with one NOP packet.
- A stalled `WAIT_REG_MEM` logs the last 40 packets (opcode, position, first arguments).
- **Renderer decision pending:** (1) translate the PM4 command stream and Xenos shaders to
  D3D12/Vulkan at runtime (Unleashed Recompiled's approach), or (2) override the game's
  statically linked D3D functions with host D3D12 calls (no command stream at all; more
  work, cleaner). Shaders need XenosRecomp either way. The game code stays native in both.

### Audio notes

- Wwise bring-up chain (traced in `hooks.cpp`): create pool `0x82520E48` -> init step
  `0x82523400` -> command queue `0x82520F08` -> its Init `0x825363A0` -> worker start
  `0x82576028`, which needs `XamNotifyCreateListener` to return a real handle.
- The render-driver callback used to block after its first frame; it was waiting on GPU
  interrupt timing and runs normally since interrupts carry the right CPU number.
- The voice engine (XHV) is created during audio setup and reads dashboard voice settings
  for user `0xFF`.

## Next steps

1. **Renderer gaps (4h):** depth buffer (mode 5 draws), indexed draws, other surface formats,
   resolves that the CPU reads back. Play further into the game to find shaders and draw types not seen yet.
2. **Instruction tests (optional):** the added opcodes were reviewed by hand; running Xenia's PPC
   tests through XenonTests would need a PowerPC assembler (binutils or an LLVM build with the PowerPC target).
