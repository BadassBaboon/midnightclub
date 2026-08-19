# Midnight Club: Los Angeles - Lightweight Static Recompilation

A lightweight static recompilation of **Midnight Club: Los Angeles (Complete Edition)** for Xbox 360, targeting Windows x86-64. Built with the [rexGlu SDK](https://github.com/rexglue/rexglue) as a compilers class project.

Static recompilation converts the original Xbox 360 PowerPC (PPC) bytecode in the game's `.xex` executable into native C++ that compiles and runs directly on a modern PC - no emulator, no interpreter. The game's kernel calls (file I/O, GPU commands, audio, threading) are handled by the rexGlu runtime.

> [!NOTE]
> This project focuses strictly on **maximum execution performance, zero bloat, and an authentic 1:1 original console gameplay experience** on modern PC hardware at 60+ FPS. Unlike alternative forks (such as LARecomp) that introduce non-standard gameplay modifications, auxiliary background polling loops, or heavy runtime hooks, all optimizations here are minimal, precision mid-asm hooks and native recompilation hints running on the pure ReXGlue engine.

---

## What Works

- Game boots and reaches the main menu cleanly
- City renders with smooth traffic and ambient life
- Save profile creation and loading work
- Free roam, garage, and race selection are fully functional
- **Correct game speed and physics** - the original 2x-at-high-framerate bug is fixed
- **Smooth frame pacing** - the 15.625 ms frame-time grid that caused constant micro-stutter is gone, giving continuous presentation
- **60+ FPS Chase Camera Continuous-Time Smoothing** - continuous exponential decay damping calibrated to 30 FPS console reference curve eliminates camera jitter and braking/turning snap
- **60+ FPS Vehicle Chassis Suspension Damping** - continuous-time roll/pitch and ground depth damping (`MCLAChassisDepthSmoothing`) guarantees authentic suspension response at all framerates
- **513 Script Native Commands Recompiled** - exhaustive binary sweep marked and compiled all 513 script native commands (HUD, UI, Warper, Message Boxes, Race Logic, Grid Spawning, Car Controls, Garage Customizers, GPS) into direct native C++ functions (0 script stub fallbacks)
- **RAGE Typed Architecture** - strongly-typed, big-endian structures (`mcla_rage_types.h`) derived from CodeX (`RSC5`) replacing fragile byte offsets
- **RAGE Jenkins Hash Symbol Diagnostics** - zero-overhead asset and symbol resolver (`mcla_symbol_resolver.h`) indexing 146,000+ engine strings
- **Hardware Cache Flush Bypass** - eliminates millions of redundant PowerPC `dcbf`/`dcbst` cache loop iterations during world streaming
- **City LOD Draw Distance Scaling** - reduces distant high-poly building vertex load in dense Downtown areas by 25%
- **Dynamic Ambient Density Tuning** - halves pedestrian crowds, moving traffic, and roadside parked cars to prevent streaming freezes
- **Optimized GPU Texture Cache Headroom** - expanded to 1536MB soft / 2048MB hard / 64MB RTT to eliminate streaming stutters and texture pop-in

## Known Issues

| Issue | Status |
|---|---|
| **Broken car reflections & object dithering** | Visual artifacts on car body reflections and dithered alpha textures (e.g. tree foliage) are due to current `rexglue` `xenos` rendering plugin limitations. Upstream [xenia-edge](https://github.com/has207/xenia-edge) has specialized rendering fixes that resolve these issues. Fixing this requires harvesting and porting those D3D12/xenos renderer improvements into `rexglue`, or waiting for a `rexglue` SDK update. |
| **Intro BIK/legal movies play fast with VSync off** | The movie player paces playback per D3D present rather than wall-clock time. Because `vsync` is kept `false` by default for maximum engine performance throughput (~30% higher framerate and eliminating the 15.625ms Windows quantization grid), intro movies render at maximum GPU speed. This is a deliberate performance trade-off. Users can press A to skip or set `MCLA_VSYNC=true` / `MCLA_FPS_CAP=60` if they prefer normal movie speed. |

See [`MCLA_workplan.md`](MCLA_workplan.md) for the full investigation log - every measurement, every hypothesis that was disproven, and what is left to do.

---

## Prerequisites

| Requirement | Notes |
|---|---|
| **rexGlue SDK v0.9.0** | Must be installed and on PATH (`rexglue` command available) |
| **CMake - 3.25** | |
| **Clang** (via LLVM or VS) | The presets use `clang`/`clang++` - MSVC will not work |
| **Ninja** | Build system used by all presets |
| **Xbox 360 game disc / extracted XEX** | You supply the game data - not included in this repo |

### Game Data Setup

Extract the game disc to a folder containing `default.xex`:

```
MCLA_Game_Files/
  default.xex
  xarchive_cache.rpf
  xarchive_audio.rpf
  ...
```

The runtime finds it automatically, first match wins:

1. `MCLA_GAME_DATA` environment variable
2. `game_data/` next to `midnightclub.exe`
3. `MCLA_Game_Files/` walking up to six levels from the working directory

No source editing required. If none match, the executable prints how to fix it
and exits cleanly rather than failing obscurely later:

```powershell
$env:MCLA_GAME_DATA = "D:\games\MidnightClubLA"
```

For **codegen only**, `midnightclub_manifest.toml` points at
`../MCLA_Game_Files/default.xex`. That is relative to the repo, so the default
layout is a `MCLA_Game_Files` folder beside your clone. Change that one line if
your layout differs - it is only read by `rexglue codegen`, never at runtime.

### Optional developer extras

Neither of these is needed to play:

- **Symbol resolution** (`MCLA_RESOLVE_SYMBOLS=1`) resolves RAGE Jenkins hashes
  to asset names for diagnostics. It needs `Codex.Games.MCLA.strings.txt` from
  [CodeX.Games.MCLA](https://github.com/Foxxyyy/CodeX.Games.MCLA). Point
  `MCLA_STRINGS_FILE` at it, drop it beside the exe, or clone CodeX next to this
  repo. Without it the resolver simply reports raw hashes.

  It is **not vendored here**: the CodeX repository ships no LICENSE file, so
  redistributing its contents is not clearly permitted. If that changes, or the
  author grants permission, vendoring the 3.6 MB file would be worthwhile.

- **`gamecontrollerdb.txt`** next to the exe improves controller mapping for
  non-Xbox pads. SDL logs a warning without it; it is otherwise harmless.

---

## Building

```powershell
# Configure (first time only)
cmake --preset win-amd64-relwithdebinfo .

# Build
cmake --build out/build/win-amd64-relwithdebinfo --target midnightclub
```

The executable is written to `out/build/win-amd64-relwithdebinfo/midnightclub.exe`.

**Visual Studio:** Open the repo folder in VS 2022, select **Windows AMD64 RelWithDebInfo** from the configuration dropdown, and click Play. The `launch.vs.json` sets the correct working directory automatically.

---

## Running

```powershell
cd out\build\win-amd64-relwithdebinfo
.\midnightclub.exe
```

On first launch the game will prompt you to create a save profile. Do it once - subsequent launches skip straight to the menu.

### Running at 60+ FPS (High Framerate)

```powershell
cd out\build\win-amd64-relwithdebinfo
$env:REX_LOG_LEVEL="warn"; $env:MCLA_FPS_CAP="60"; Start-Process midnightclub.exe
```

**60+ FPS is fully supported and smooth.** All chase camera trailing/lag calculations, steering caster return, suspension spring/damper dynamics, and chassis roll/pitch ground depth filters are stepped using continuous-time exponential decay equations ($S(dt)$ and $\alpha(dt)$) calibrated to the authentic 30 FPS console reference.

### Running Uncapped or at 120/144/240 Hz

```powershell
$env:REX_LOG_LEVEL="warn"; Start-Process midnightclub.exe
```

Leaving `MCLA_FPS_CAP` unset allows the engine to run uncapped at your monitor's full native refresh rate (e.g. 144 Hz or 240 Hz) with complete physical consistency and 0 quantization grid stutter.

### Running at 30 FPS (Console Baseline)

```powershell
$env:REX_LOG_LEVEL="warn"; $env:MCLA_FPS_CAP="30"; Start-Process midnightclub.exe
```

For users wanting the exact original 30 FPS console framerate, `MCLA_FPS_CAP=30` enforces a wall-clock frame-time deadline with continuous time spacing.

### Environment variables

| Variable | Default | Purpose |
|---|---|---|
| `MCLA_FPS_CAP` | unset (uncapped) | Frame rate limit. Uncapped by default. Set to `60` or `30` if desired. |
| `REX_LOG_LEVEL` | `trace` on non-Release builds | **Set to `warn`.** The default costs ~7,500 log lines/sec and ~1.4 MB/s of synchronous disk I/O during gameplay. |
| `MCLA_LOD_CITY_SCALE` | `0.75` | City & building LOD draw distance multiplier (`0.1` to `10.0`). Reduces geometry draw call pressure in Downtown by 25%. |
| `MCLA_TRAFFIC_DENSITY_SCALE` | `0.5` | Ambient moving traffic density multiplier (`0.1` to `2.0`). Halves background traffic to eliminate async streaming queue bottlenecks. |
| `MCLA_PED_DENSITY_SCALE` | `0.5` | Pedestrian spawn density multiplier (`0.0` to `2.0`). Halves sidewalk crowd population. |
| `MCLA_PARKED_CAR_SCALE` | `0.5` | Roadside parked car density multiplier (`0.0` to `2.0`). Halves parked vehicle counts. |
| `MCLA_TRAFFIC_UNSPAWN_MAX` | `180.0` | Max traffic unspawn tracking radius (meters). Reduces distant CPU vehicle tracking overhead. |
| `MCLA_DISABLE_DOF` | `1` (enabled) | Disable full-screen Depth of Field composite passes, saving GPU fill rate and improving clarity. Set to `0` to re-enable DoF blur. |
| `MCLA_SKIP_INTRO` | `0` (off) | Set to `1` to skip the intro legal movies. Also clears render-pass bit 24, which otherwise leaves an uninitialised pass that corrupts Downtown shaders. |
| `MCLA_RT_PATH` | auto | eDRAM path: `d3d12_rov` or `d3d12_rtv`. Unset lets the plugin choose. |
| `MCLA_RESOLUTION_SCALE` | `1` | **Do not change.** `2` breaks race-start cameras and grid spawn transforms. |
| `MCLA_ALLOW_INVALID_FETCH` | `true` | Allows texture fetches the GPU log flags as invalid. Set `false` if HUD/minimap glitches appear. |
| `MCLA_CAMERA_SMOOTH_SCALE` | `1.0` | Chase-camera smoothing rate multiplier (`0.1`-`5.0`). |
| `MCLA_REFRESH_RATE` | `60` | Guest video mode refresh rate. Diagnostic only - does not affect pacing. |
| `MCLA_SUBSTEPS` | unset | Diagnostic override of the physics substep count (`1`-`8`). Leave alone. |
| `MCLA_DISABLE_IMPOSTER_SHADOWS` | `1` (enabled) | Bypass tree foliage imposter shadow submission, saving GPU fill rate in Beverly Hills / Hollywood. |
| `MCLA_DISABLE_MOTION_BLUR` | `0` (off) | Set to `1` to disable camera and per-object motion blur passes. |
| `MCLA_DISABLE_MSAA` | `0` (off) | Set to `1` to disable hardware MSAA. |
| `MCLA_TEX_SOFT` | `1536` | GPU texture cache soft memory limit (MB). Expanded to prevent cache thrashing in dense city areas. |
| `MCLA_TEX_HARD` | `2048` | GPU texture cache hard memory limit (MB). |
| `MCLA_TEX_RTT` | `64` | GPU texture cache limit for render-to-texture targets (MB). |
| `MCLA_RESOLVE_SYMBOLS` | `0` (off) | Set to `1` (or run with `REX_LOG_LEVEL=debug`) to enable lazy RAGE Jenkins hash string resolution for diagnostics. Zero overhead when off. |
| `MCLA_TILED_SHARED` | `false` | Disables tiled shared memory for lower emulation overhead on modern GPUs. |
| `MCLA_GAME_DATA` | auto-detect | Path to the folder containing `default.xex`. Overrides auto-detection. |
| `MCLA_STRINGS_FILE` | auto-detect | Path to `Codex.Games.MCLA.strings.txt` for symbol resolution. Optional. |
| `MCLA_NO_STUB_SWEEP` | `0` | `1` skips stubbing ~1.7M unmapped addresses at startup (~400 ms). Only safe if `stubs.txt` stays empty. |
| `MCLA_TIMING_LOG` | off | `1` writes frame-time stats and a 1 ms histogram to `logs/timing_<date>_<time>_cap<N>.log`. |
| `MCLA_MAX_FRAME_MS` | `125` | Hitch guard: caps the delta a single frame can advance. Clamped to `[16, 1000]`; cannot be disabled. |
| `MCLA_VSYNC` | `false` | Presentation vsync lock. `false` (default) gives maximum performance throughput (~30% higher framerate). Set to `true` for vsync lock. |
| `MCLA_NO_TIMER_RES` | unset | `1` skips `timeBeginPeriod(1)`. Restores the 15.625 ms grid - for A/B comparison only. |
| `MCLA_PRESENT_INTERVAL` | forces `1` | `orig` keeps the guest's value. **Do not use** - it causes shadow/lighting flicker and does not slow the movies. |

`REX_LOG_LEVEL` must be an environment variable. Setting the log level from
`OnPostInitLogging()` does not work - the runtime has already emitted its
startup banner by then.

---

## If You Re-Run Codegen

```powershell
rexglue codegen midnightclub_manifest.toml
cmake --build out/build/win-amd64-relwithdebinfo --target midnightclub
```

Note the manifest, not the config - `midnightclub_manifest.toml` is the codegen
entry point and it `includes` `midnightclub_config.toml`.

**Nothing under `generated/` is hand-edited, and it must stay that way.**
Codegen rewrites that entire directory. All engine patches live as
`[[midasm_hook]]` entries in `midnightclub_config.toml`, with implementations in
`src/midnightclub_hooks.cpp`, so they survive regeneration. If you need a new
patch, add a hook - do not edit `generated/`.

---

## How It Works / Fixes Applied

The rexGlu SDK translates each PPC function in the XEX into a C++ function. Functions not in the hint list get a no-op stub registered at runtime. Several things needed fixing before the game would run:

| Problem | Fix |
|---|---|
| Static initializers calling unregistered functions | Pass 1: scan the XEX init tables and stub any missing entries |
| "Dirty Disc" error shown for extracted files | Bypass `sub_82130678` (the disc-error handler) with a no-op |
| Indirect calls to any unregistered code address | Pass 2: stub the entire code region `[0x82130000, 0x827CD054]` |
| `mullhwu.` / vmx128 opcodes misidentified as ppc405 | Fixed in rexGlu - 0.7.8 codegen - re-run codegen to pick up the fix |
| `t:` drive not registered in VFS | Register `t:` - `\Device\Harddisk0\Partition1` so city art/data lookups resolve |
| Crash log lost when `abort()` is called | SIGABRT handler in `OnPostSetup` writes a stack trace to `crash_stack.txt` before dying |

### Timing and Performance Fixes

| Problem | Fix |
|---|---|
| Game ran at 2x speed above 30 fps | `MCLAUseRealDelta` hook at `0x821BDB08`. The engine had a fixed-timestep path that discarded the measured delta. Unlike Xenia's patch, this keeps the "timer was reset" guard at `[r3+56]` - skipping it unconditionally feeds a garbage delta on reset frames and desynchronises the audio threads. |
| Present interval locked to 30 Hz | `MCLAPresentInterval` hook at `0x82419AA0`, a PM4 packet field, `2` -> `1` vblanks. |
| Frame times quantized to a 15.625 ms grid | `timeBeginPeriod(1)` plus `vsync=false`. The grid was Windows' default timer granularity, not the display and not the guest vblank rate. Both changes are required; either alone leaves the grid intact. ~30% throughput gain. |
| Nothing throttled presentation once vsync was off | Time-based frame limiter (`MCLA_FPS_CAP`) with a wall-clock deadline. Deliberately not vblank-based, which would reintroduce quantization. |
| Physics exploding after a streaming stall | `MCLAFrameDelta` hook at `0x821BDAB0` clamps the per-frame delta (`MCLA_MAX_FRAME_MS`, default 125 ms). |
| Chase camera jitter at 60 FPS | `MCLACameraBoomSmoothing` hook at `0x823203D4` applies rate-invariant exponential decay `1 - pow(1 - k, dt * 30)` before `bl sub_8231D3A8`, eliminating 60 FPS camera snapping. |
| Hardware cache flush loop hitching during streaming | `mc_FlushDataCache` hook at `0x821D5510` returns immediately on PC, eliminating 2.5M+ redundant `dcbf`/`dcbst` loop iterations every 2 seconds. |
| Downtown geometry draw call bottleneck | `Patch_ScaleCityLOD` hook at `0x822D5BC4` and `UpdateCityLODMemory` (`0x827E0DE0`) scale distant building LOD transitions by 25% (`MCLA_LOD_CITY_SCALE=0.75`), cutting vertex workload. |
| Ambient streaming queue overflows | `MCLAAmbientDensityTuning` hook at `0x826F5CA0` halves moving traffic, pedestrians, and parked cars to prevent synchronous engine freezes. |
| Vehicle steering sensitivity doubling at 60 FPS | `MCLATurnSpeedTimestep` hook at `0x822A2ED4` scales steering timestep factor by `0.5` at 60 FPS to keep turn response console-accurate. |
| Texture cache thrashing causing city slowdowns | Expanded texture cache limits (`soft=768`, `hard=1024`) in `ApplyGpuFlags`, halving severe frame drops (>50ms) in dense city areas. |
| Entire GPU config silently ignored | Cvars in `rex/graphics/flags.h` live in the xenos plugin DLL, which loads *after* `OnPreSetup` returns. They must be set from `OnPostSetup`. `SetFlagByName` returns `false` in that case, so typos and mistimed calls look identical to success - the app now logs every attempt. |

### Debugging Aids

- **`logs/effective_config.txt`** - written at startup. The *actual* value of every cvar we care about, plus an `ok`/`FAIL` line per `SetFlagByName` attempt. Check this first when a setting appears to have no effect.
- **`logs/timing_<date>_<time>_cap<N>.log`** - with `MCLA_TIMING_LOG=1`. Per-second frame rate and spike counts, plus a 1 ms-resolution frame-time histogram every 10 s. The histogram is what proves whether pacing is quantized or continuous.
- **`stubs.txt`** - every call to a stubbed PPC address logs `addr` + `LR`. The last entry before a crash tells you which unimplemented function was called.
- **`crash_stack.txt`** - written on `abort()`. Full call stack from the crash site.

---

## Project Structure

```
midnightclub/
  src/
    main.cpp                 # Entry point (rexGlu-generated, do not edit)
    midnightclub_app.h       # App customization: paths, cvars, stubs, VFS setup
    midnightclub_hooks.cpp   # Mid-asm hook implementations (timing fixes)
  generated/                 # REGENERATED BY CODEGEN - never hand-edit
    midnightclub_init.h      # Runtime macros and REX_CALL_INDIRECT_FUNC
    midnightclub_recomp.*.cpp  # PPC - C++ translated game code (60+ files)
    rexglue.cmake            # SDK CMake integration
  midnightclub_manifest.toml # Codegen entry point (XEX path, includes config)
  midnightclub_config.toml   # Function hints + [[midasm_hook]] declarations
  CMakeLists.txt
  CMakePresets.json
  launch.vs.json             # Visual Studio debug launch config
  RUNNING.md                 # Quick-reference run guide
```

### Mid-asm hooks

Engine patches are declared in `midnightclub_config.toml`:

```toml
[[midasm_hook]]
name = "MCLAUseRealDelta"
address = 0x821BDB08
registers = ["cr6"]
jump_address_on_true = 0x821BDC34
```

and implemented in `src/midnightclub_hooks.cpp`. Codegen emits a call at that
guest address. The function signature takes **only the registers named in
`registers`, by reference**, with ordinary C++ linkage - not `extern "C"`, and
not `(ctx, base)`. Returning `bool` drives `jump_address_on_true`.

---

## Credits & Acknowledgments

- **[Foxxyyy](https://github.com/Foxxyyy)**: Outstanding reverse-engineering work on **[CodeX](https://github.com/Foxxyyy)** (`CodeX.Games.MCLA`), providing the RAGE `RSC5` resource format specifications, type layouts, string databases, and data structures that made the typed architecture and symbol resolver in this project possible.
- **[ReXGlue Team](https://github.com/rexglue/rexglue)**: The Xbox 360 static recompilation toolkit and runtime.
- **Rockstar San Diego**: The original developers and creators of Midnight Club: Los Angeles.
