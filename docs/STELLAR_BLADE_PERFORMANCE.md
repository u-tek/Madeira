# Stellar Blade performance notes

Stellar Blade is the heaviest game Madeira runs: Unreal Engine 4, Direct3D 12 only,
a 345 MB executable with no relocations, about 1,300 threads, and Denuvo. These notes
collect what was changed for it, how to set it up, what to check on the phone, and the
larger ideas that are not built yet.

None of this has been measured on a device yet. Every change below is either a pure
removal of work or sits behind a switch, so each can be A/B tested.

## What changed

| Change | Where | Switch |
|---|---|---|
| The two built-in profilers that `thread_suspend` guest threads are off by default. The W-thread profiler suspended the render and worker threads 1,000 times a second in ~3 s bursts every ~18 s; the thread sampler and its rip-profile suspended every live thread every 20 s. The passive `[xp]` probe stays on. | `build/ntdll-unix/server_ios.c` | `env.MADEIRA_PROFILERS` = `1` (all), unset (passive only), `0` (none) |
| A thread spinning on `GetCompletedValue` of an unchanged fence waits on the fence instead. A code comment records UE's RHI submission thread at 4.4 M calls/s, ~93% of a performance core, and FSR 3 frame generation at up to 14,000 polls a frame. After 64 polls in a row, each under 4 µs apart, the poll sleeps on the fence's condition variable: woken the moment the value moves, never held over 1 ms. The frame-generation stall report still fires (a wait counts as 1,024 polls). | `madeira-d3d12/src/pe/madeira_d3d12.c` | `fence-poll-wait = 0` restores pure polling |
| Draw and dispatch replay no longer re-declare the command list's last 64 used resources on every draw. Each candidate scanned up to 96 declarations, ~3,000 compares per draw on the replay the render thread waits for. A declaration holds for the rest of its encoder, so the loop now runs once per encoder, keyed on the encoder, the list's tail length, its handles and the stages. | `madeira-d3d12/src/pe/madeira_d3d12.c` | `use-dedup = 0` restores a declaration per call, as before |
| FEX's L1-miss path (`CompileBlock`, ~15,000 calls a frame, nearly all cache hits) did 2-3 process-wide atomic read-modify-writes per call. It now samples 1 call in 64. | `build/fex-arm64ec/0003-hot-path-diagnostics.patch` | none (diagnostics only; `[CB_SUMMARY]` totals are now estimates) |
| FEX's per-call callret table and the HOTRIP counters are compiled out. Besides the shared writes on every L1 miss, the HOTRIP block is keyed on fixed RIPs of an earlier game in the 0x140000000 range, where Stellar Blade's executable lives; at 0x140006feb it dereferenced guest RBX/RCX inside the compiler. | same patch | define `MADEIRA_FEX_HOTRIP_DIAG` to bring them back |
| The process-wide L1-miss counter (`[l1-miss]`) is added to 256 at a time from each thread's own count. | same patch | none |
| Smaller ones: the winios input drain logged and flushed every input event on the window's thread (now 1 in 30); win32u read `MADEIRA_DESKTOP` with `getenv` on every window-proc call (now once); the guest-caller dump on SMC writes did two Mach reads before its rate limit (now after). | `app/Madeira/Winios/Winios.m`, `build/win32u-unix/message_ios.c`, `build/ntdll-unix/signal_arm64_ios.c` | none |

## Recommended madeira.cfg

```
# Second JIT code region for FEX's code buffers. Off by default; the code comments tie
# Stellar Blade's retranslation stalls (~4.4M retranslations, ~2 fps while busy, a drop
# to 2-12 fps each time a 64 MB generation filled) to the pool's small code buffers.
env.MADEIRA_JIT_CODE_REGION = 1

# Both on by default; listed so an A/B is one line away.
env.MADEIRA_FEX_FASTCALLS = 1        # TlsGetValue, critical sections, GetCurrentThreadId inline
env.MADEIRA_FEX_SPIN_DEPRESS = 1     # QPC busy-wait priority depress
fence-poll-wait = 1

# Leave unset for play. Set to 1 only when collecting [thread-sample]/[wprof]/rip-profile.
# env.MADEIRA_PROFILERS = 1
```

Leave `MADEIRA_POOL_HIGH` off: the code records that pool code does not execute there on
an iPhone 17 Pro. Leave `eco` off: it lowers the guest threads' QoS.

Per game, in Game details: keep the FPS limit at 30 or 60 locked rather than uncapped. A
phone that stays cooler keeps its performance cores; an uncapped game heats it until iOS
parks them.

## Engine.ini profile

Unreal reads console variables from the user config, and Stellar Blade honours them
(Steam Deck players use the same file). The file is in the Wine prefix:

```
drive_c/users/<user>/AppData/Local/SB/Saved/Config/WindowsNoEditor/Engine.ini
```

Some guides give `Windows` instead of `WindowsNoEditor` as the last folder; use whichever
exists. Back up the original, append the block below, and if the game rewrites the file on
start, mark it read-only.

Most of these cut draw calls as well as GPU time. On Madeira every draw costs translated
game code, the D3D12 runtime and the Metal replay, so fewer draws is the biggest CPU lever
the game itself offers.

```
[SystemSettings]
; fewer objects and draws
r.ViewDistanceScale=0.6
foliage.DensityScale=0.4
grass.DensityScale=0.4
r.StaticMeshLODDistanceScale=1.5
r.SkeletalMeshLODBias=1
r.DetailMode=1
a.URO.Enable=1
; shadows: each cascade is another depth pass of draws
r.Shadow.CSM.MaxCascades=2
r.Shadow.MaxResolution=1024
r.Shadow.MaxCSMResolution=1024
r.Shadow.RadiusThreshold=0.05
r.Shadow.DistanceScale=0.7
r.ContactShadows=0
r.CapsuleShadows=0
; GPU-heavy effects
r.VolumetricFog=0
r.SSR.Quality=0
r.AmbientOcclusionLevels=1
r.DepthOfFieldQuality=0
r.MotionBlurQuality=0
r.LensFlareQuality=0
r.SceneColorFringeQuality=0
r.LightShaftQuality=0
r.BloomQuality=2
r.Tonemapper.GrainQuantization=0
r.MaxAnisotropy=4
```

These are standard UE 4.26 variables chosen for a phone; none has been tried on a device.
If something looks wrong, remove lines in groups to find it.

## Checking it on the phone

| Look for | Means |
|---|---|
| `[profilers] MADEIRA_PROFILERS=(unset): passive probe on, suspending samplers off` | the samplers are off |
| `[madeira-d3d12] fence-poll-wait = 1 (...)` | a thread was caught spinning on a fence and now waits (printed once, the first time) |
| `[perf] ml1109 sync per frame: ... GetCompletedValue polls N` | N should fall from thousands to tens with `fence-poll-wait = 1` |
| `[fg-diag] ml1270 STALL` | still reported for a stuck frame-generation fence, about 1 s in |

A/B pairs worth a run each, same save and spot, 2-3 minutes:

1. `fence-poll-wait = 0` against `1`.
2. `env.MADEIRA_PROFILERS = 1` against unset.
3. `env.MADEIRA_JIT_CODE_REGION = 1` against unset (watch for the `[jit2]` line saying where the region landed).
4. With and without the Engine.ini block.

## Larger ideas, not built

These came out of the same sweep. Each needs device testing that was not possible here.

1. **Keep still-valid translations when a code buffer is replaced.** Gameplay filled a
   64 MB FEX generation every 10-30 s. Steady gameplay should not need that much new code,
   so most of it is likely re-translation of invalidated pages. FEX already records
   relocations per block; copying the live blocks of the old generation into the new one,
   instead of starting empty, would turn each 2-12 fps drop into a copy.
2. **Take the game's x64 DLLs out of the JIT pool.** The pool holds image copies (~530 MB
   before the first frame) and FEX's code buffers share what is left. Pure-x64 code is
   never executed natively, only translated, and the main executable already runs from
   outside the pool. ml457 tried skipping the copies and failed because other code assumed
   pool addresses; doing it consistently would free hundreds of MB for code.
3. **Patch faulting low-address accesses at translation time.** Every guest access below
   4 GB (KUSER_SHARED_DATA at 0x7ffe0000, sub-floor images) faults into the Mach handler.
   Recording the guest RIP of each such fault and recompiling that block with an inline
   address fix-up (as FEX already does for unaligned atomics) would make repeat accesses a
   few instructions.
4. **Cheaper call-return predictor resets.** Each reset is a VirtualQuery, a decommit and a
   commit of 16 MB through Wine's memory lock, per thread, on every code-buffer rollover and
   every invalidation that touches the thread's cache. With ~1,300 threads that lands
   exactly when the game is already stalled.
5. **Route the game's upscaler and frame generation to MetalFX.** Stellar Blade feeds FSR
   colour, depth, motion vectors and jitter through the FidelityFX API. A native
   replacement for that DLL could hand them to MetalFX's temporal scaler and iOS 26's frame
   interpolator instead of running FSR's compute shaders through the shader converter.
6. **Thread QoS by Unreal thread name.** UE names its threads (GameThread, RenderThread,
   RHIThread, TaskGraph workers, pool threads). Mapping those names to QoS classes would
   keep the frame-critical threads on the performance cores.
