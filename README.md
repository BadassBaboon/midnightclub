# Midnight Club: Los Angeles — Static Recompilation

![Demo](assets/demo.gif)

A static recompilation of **Midnight Club: Los Angeles (Complete Edition)** for Xbox 360, targeting Windows x86-64. Built with the [rexGlu SDK](https://github.com/rexglue/rexglue) as a compilers class project.

Static recompilation converts the original Xbox 360 PowerPC (PPC) bytecode in the game's `.xex` executable into native C++ that compiles and runs directly on a modern PC — no emulator, no interpreter. The game's kernel calls (file I/O, GPU commands, audio, threading) are handled by the rexGlu runtime.




---

## What Works

- Game boots and reaches the main menu
- City renders with traffic
- Save profile creation works
- Free roam / race selection is reachable
- **Correct game speed and physics** — the original 2x-at-high-framerate bug is fixed
- **Smooth frame pacing** — the 15.625 ms frame-time grid that caused constant
  micro-stutter is gone, worth roughly 30% throughput

## Known Issues

| Issue | Status |
|---|---|
| **Broken car reflections & object dithering** | Visual artifacts on car body reflections and dithered alpha textures (e.g. tree foliage) are due to current `rexglue` `xenos` rendering plugin limitations. Upstream [xenia-edge](https://github.com/has207/xenia-edge) has specialized rendering fixes that resolve these issues. Fixing this requires harvesting and porting those D3D12/xenos renderer improvements into `rexglue`, or waiting for a `rexglue` SDK update. |
| Camera and traffic *feel* faster above 30 fps | Simulation speed is rate-invariant (measured); chase camera boom is fixed (`0x823203D4`), but some secondary subsystems still interpolate per-frame. Vehicle handling & SUV slip damping were preserved as a deliberate compromise. `MCLA_FPS_CAP=30` remains recommended for the most console-faithful experience. |
| Intro `.bik` movies play too fast | Independent of frame rate — the movie player has its own timing path. Skippable. |
| Frame rate drops in dense city areas | Mitigated via expanded GPU texture cache (`soft=768`, `hard=1024`), which halved severe >50ms frame spikes. Remaining drops are CPU-bound in the recompiled code. |

See [`MCLA_workplan.md`](MCLA_workplan.md) for the full investigation log —
every measurement, every hypothesis that was disproven, and what is left to do.

---

## Prerequisites

| Requirement | Notes |
|---|---|
| **rexGlue SDK v0.9.0** | Must be installed and on PATH (`rexglue` command available) |
| **CMake ≥ 3.25** | |
| **Clang** (via LLVM or VS) | The presets use `clang`/`clang++` — MSVC will not work |
| **Ninja** | Build system used by all presets |
| **Xbox 360 game disc / extracted XEX** | You supply the game data — not included in this repo |

### Game Data Setup

Extract the game disc to a folder. The structure should look like:

```
Midnight Club - Los Angeles - Complete Edition (USA, Europe)/
  default.xex
  xarchive_cache.rpf
  xarchive_audio.rpf
  ...
```

Then update the two paths in these files to point at your copy:

- `midnightclub_manifest.toml` → `[entrypoint] file_path` (path to `default.xex`)
- `src/midnightclub_app.h` → `OnConfigurePaths`, `paths.game_data_root` (the folder containing `default.xex`)

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

On first launch the game will prompt you to create a save profile. Do it once — subsequent launches skip straight to the menu.

### Recommended: run at 30 fps

```powershell
cd out\build\win-amd64-relwithdebinfo
$env:REX_LOG_LEVEL="warn"; $env:MCLA_FPS_CAP="30"; Start-Process midnightclub.exe
```

**30 fps is fully correct and is the recommended way to play.** Physics,
steering, collisions, camera and traffic all behave as they do on a real
console, and frame pacing is smooth and continuous — no stutter, no 2x speed.

The 30 fps cap is *not* the old 30 fps lock. The original game hit 30 fps by
waiting on vblank, which quantized every frame to a 15.625 ms grid and produced
constant micro-stutter. The cap here is a time-based limiter, so frame times are
continuous and evenly spaced.

### 60 fps

```powershell
$env:REX_LOG_LEVEL="warn"; $env:MCLA_FPS_CAP="60"; Start-Process midnightclub.exe
```

60 fps runs and is stable, and the simulation advances at the correct speed
(verified by measurement). However the camera and traffic still *feel* faster,
because some subsystems smooth their motion by a per-frame constant rather than
by elapsed time — at double the frame rate those settle twice as fast. Playable,
but 30 fps is the more faithful experience today.

Leave `MCLA_FPS_CAP` unset to run uncapped. Not recommended: the game reaches
70-100+ fps in light scenes, which exaggerates the effects above.

### Environment variables

| Variable | Default | Purpose |
|---|---|---|
| `MCLA_FPS_CAP` | unset (uncapped) | Frame rate limit. `30` recommended. |
| `REX_LOG_LEVEL` | `trace` on non-Release builds | **Set to `warn`.** The default costs ~7,500 log lines/sec and ~1.4 MB/s of synchronous disk I/O during gameplay. |
| `MCLA_TEX_SOFT` | `768` | GPU texture cache soft memory limit (MB). Expanded to prevent cache thrashing in dense city areas. |
| `MCLA_TEX_HARD` | `1024` | GPU texture cache hard memory limit (MB). |
| `MCLA_TEX_RTT` | `0` | GPU texture cache limit for render-to-texture targets (MB). |
| `MCLA_TILED_SHARED` | `false` | Disables tiled shared memory for lower emulation overhead on modern GPUs. |
| `MCLA_RESOLUTION_SCALE` | `1` | Internal 3D resolution multiplier. **Keep at `1`** — setting to `2` corrupts projection frustum culling and grid spawn transforms. |
| `MCLA_TIMING_LOG` | off | `1` writes frame-time stats and a 1 ms histogram to `logs/timing_<date>_<time>_cap<N>.log`. |
| `MCLA_MAX_FRAME_MS` | `125` | Hitch guard: caps the delta a single frame can advance. Clamped to `[16, 1000]`; cannot be disabled. |
| `MCLA_VSYNC` | `false` | `true` restores vsync. Reintroduces frame-time quantization. |
| `MCLA_NO_TIMER_RES` | unset | `1` skips `timeBeginPeriod(1)`. Restores the 15.625 ms grid — for A/B comparison only. |
| `MCLA_PRESENT_INTERVAL` | forces `1` | `orig` keeps the guest's value. **Do not use** — it causes shadow/lighting flicker and does not slow the movies. |

`REX_LOG_LEVEL` must be an environment variable. Setting the log level from
`OnPostInitLogging()` does not work — the runtime has already emitted its
startup banner by then.

---

## If You Re-Run Codegen

```powershell
rexglue codegen midnightclub_manifest.toml
cmake --build out/build/win-amd64-relwithdebinfo --target midnightclub
```

Note the manifest, not the config — `midnightclub_manifest.toml` is the codegen
entry point and it `includes` `midnightclub_config.toml`.

**Nothing under `generated/` is hand-edited, and it must stay that way.**
Codegen rewrites that entire directory. All engine patches live as
`[[midasm_hook]]` entries in `midnightclub_config.toml`, with implementations in
`src/midnightclub_hooks.cpp`, so they survive regeneration. If you need a new
patch, add a hook — do not edit `generated/`.

---

## How It Works / Fixes Applied

The rexGlu SDK translates each PPC function in the XEX into a C++ function. Functions not in the hint list get a no-op stub registered at runtime. Several things needed fixing before the game would run:

| Problem | Fix |
|---|---|
| Static initializers calling unregistered functions | Pass 1: scan the XEX init tables and stub any missing entries |
| "Dirty Disc" error shown for extracted files | Bypass `sub_82130678` (the disc-error handler) with a no-op |
| Indirect calls to any unregistered code address | Pass 2: stub the entire code region `[0x82130000, 0x827CD054]` |
| `mullhwu.` / vmx128 opcodes misidentified as ppc405 | Fixed in rexGlu ≥ 0.7.8 codegen — re-run codegen to pick up the fix |
| `t:` drive not registered in VFS | Register `t:` → `\Device\Harddisk0\Partition1` so city art/data lookups resolve |
| Crash log lost when `abort()` is called | SIGABRT handler in `OnPostSetup` writes a stack trace to `crash_stack.txt` before dying |

### Timing and Performance Fixes

| Problem | Fix |
|---|---|
| Game ran at 2x speed above 30 fps | `MCLAUseRealDelta` hook at `0x821BDB08`. The engine had a fixed-timestep path that discarded the measured delta. Unlike Xenia's patch, this keeps the "timer was reset" guard at `[r3+56]` — skipping it unconditionally feeds a garbage delta on reset frames and desynchronises the audio threads. |
| Present interval locked to 30 Hz | `MCLAPresentInterval` hook at `0x82419AA0`, a PM4 packet field, `2` → `1` vblanks. |
| Frame times quantized to a 15.625 ms grid | `timeBeginPeriod(1)` plus `vsync=false`. The grid was Windows' default timer granularity, not the display and not the guest vblank rate. Both changes are required; either alone leaves the grid intact. ~30% throughput gain. |
| Nothing throttled presentation once vsync was off | Time-based frame limiter (`MCLA_FPS_CAP`) with a wall-clock deadline. Deliberately not vblank-based, which would reintroduce quantization. |
| Physics exploding after a streaming stall | `MCLAFrameDelta` hook at `0x821BDAB0` clamps the per-frame delta (`MCLA_MAX_FRAME_MS`, default 125 ms). |
| Chase camera jitter at 60 FPS | `MCLACameraBoomSmoothing` hook at `0x823203D4` applies rate-invariant exponential decay `1 - pow(1 - k, dt * 30)` to chase camera boom interpolation constant `f1`. |
| Texture cache thrashing causing city slowdowns | Expanded texture cache limits (`soft=768`, `hard=1024`) in `ApplyGpuFlags`, halving severe frame drops (>50ms) in dense city areas. |
| Entire GPU config silently ignored | Cvars in `rex/graphics/flags.h` live in the xenos plugin DLL, which loads *after* `OnPreSetup` returns. They must be set from `OnPostSetup`. `SetFlagByName` returns `false` in that case, so typos and mistimed calls look identical to success — the app now logs every attempt. |

### Debugging Aids

- **`logs/effective_config.txt`** — written at startup. The *actual* value of every cvar we care about, plus an `ok`/`FAIL` line per `SetFlagByName` attempt. Check this first when a setting appears to have no effect.
- **`logs/timing_<date>_<time>_cap<N>.log`** — with `MCLA_TIMING_LOG=1`. Per-second frame rate and spike counts, plus a 1 ms-resolution frame-time histogram every 10 s. The histogram is what proves whether pacing is quantized or continuous.
- **`stubs.txt`** — every call to a stubbed PPC address logs `addr` + `LR`. The last entry before a crash tells you which unimplemented function was called.
- **`crash_stack.txt`** — written on `abort()`. Full call stack from the crash site.

---

## Project Structure

```
midnightclub/
  src/
    main.cpp                 # Entry point (rexGlu-generated, do not edit)
    midnightclub_app.h       # App customization: paths, cvars, stubs, VFS setup
    midnightclub_hooks.cpp   # Mid-asm hook implementations (timing fixes)
  generated/                 # REGENERATED BY CODEGEN — never hand-edit
    midnightclub_init.h      # Runtime macros and REX_CALL_INDIRECT_FUNC
    midnightclub_recomp.*.cpp  # PPC → C++ translated game code (60+ files)
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
`registers`, by reference**, with ordinary C++ linkage — not `extern "C"`, and
not `(ctx, base)`. Returning `bool` drives `jump_address_on_true`.
