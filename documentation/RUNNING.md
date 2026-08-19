# How to Run Midnight Club LA (Recompiled)

## Quick Start

```powershell
cd out\build\win-amd64-relwithdebinfo
$env:REX_LOG_LEVEL="warn"; Start-Process midnightclub.exe
```

That is it. The game finds its data automatically, runs uncapped at your
monitor's refresh rate, and the simulation stays correct at any frame rate.

**Always set `REX_LOG_LEVEL=warn`.** Non-Release builds default to `trace`,
which costs ~7,500 log lines/sec and ~1.4 MB/s of synchronous disk I/O during
gameplay. That is a genuine stutter source, not just noise.

On first launch the game asks you to create a save profile. Do it once.

**Visual Studio:** open the repo folder, pick **Windows AMD64 RelWithDebInfo**,
click Play. Note this does not set the environment variables above, so you get
trace-level logging - fine for debugging, not for judging performance.

## Frame rate

60+ FPS is fully supported. Chase camera lag, steering, suspension damping and
the chassis ground-depth filter all use continuous-time exponential decay
calibrated to the 30 FPS console reference, so behaviour matches the original at
30, 60, 120, 144 and beyond.

```powershell
# cap to a specific rate
$env:MCLA_FPS_CAP="60"; Start-Process midnightclub.exe

# original console frame rate
$env:MCLA_FPS_CAP="30"; Start-Process midnightclub.exe
```

The cap is a wall-clock limiter, not a vblank wait, so frame times stay evenly
spaced. This is not the old 30 FPS lock: the original reached 30 by waiting on
vblank, which quantized frame times to a 15.625 ms grid and produced constant
micro-stutter.

See the README for the full environment-variable table.

---

## Build from Scratch

```powershell
cmake --build out/build/win-amd64-relwithdebinfo --target midnightclub
```

Reconfigure first if needed:

```powershell
cmake --preset win-amd64-relwithdebinfo .
cmake --build out/build/win-amd64-relwithdebinfo --target midnightclub
```

## If You Re-Run `rexglue codegen`

```powershell
rexglue codegen midnightclub_manifest.toml
cmake --build out/build/win-amd64-relwithdebinfo --target midnightclub
```

Note the **manifest**, not the config - `midnightclub_manifest.toml` is the
codegen entry point and it includes `midnightclub_config.toml`.

**Nothing under `generated/` is hand-edited.** Codegen rewrites that whole
directory. All engine patches are `[[midasm_hook]]` entries in
`midnightclub_config.toml`, implemented in `src/midnightclub_hooks.cpp`, so they
survive regeneration. Add a hook - never edit `generated/`.

## Save Data

Stored under `user_data\` in the build directory. To reset to first-run state,
delete the profile folder there.

---

## Debugging

- **`logs\effective_config.txt`** - the *actual* value of every cvar we care
  about, an `ok`/`FAIL` line per `SetFlagByName` attempt, every `MCLA_*`
  environment variable, and the stub-sweep timing. Check this first whenever a
  setting appears to have no effect. GPU cvars only bind from `OnPostSetup`,
  never from `OnPreSetup`.
- **`logs\timing_<date>_<time>_cap<N>.log`** - with `MCLA_TIMING_LOG=1`. One
  file per run, so back-to-back runs do not overwrite each other. Contains:
  - per-second frame rate and spike counts (>20/33/50/100 ms)
  - a 1 ms-resolution frame-time histogram every 10 s
  - `SIM RATE` - the frame-rate-independence regression check. Should read
    ~2.00x at **every** cap. A value that changes with frame rate means a timing
    hook has regressed.
  - `ACCUM` - the engine's accumulated-time totals, should advance ~1.0/s
  - `perf:` - live runtime counters (`buffer_queue_depth`,
    `texture_cache_hits/misses`, `draw_calls`, `active_threads`)
- **`stubs.txt`** - every stubbed PPC function call (addr + LR). Empty in normal
  operation; the last entry before a crash is what was called.
- **`crash_stack.txt`** - written on `abort()`, full call stack.

Note several perf counters are compiled out of the shipped runtime DLL and read
zero always: `xma_frames_decoded`, `audio_frame_latency_us`,
`command_buffer_stalls`, `critical_region_contentions`, `apc_queue_depth`. Do
not read meaning into those zeros.

---

## What Was Fixed

### Getting it to boot

| Problem | Fix |
|---|---|
| Static initializers calling unregistered functions | Scan the XEX init tables and stub missing entries |
| "Dirty Disc" error on extracted files | Bypass `sub_82130678` with a no-op |
| Indirect calls to unregistered addresses | Stub the code region `[0x82130000, 0x827CD054]`. ~1.7M entries, ~400-500 ms at startup; `MCLA_NO_STUB_SWEEP=1` skips it |
| `t:` drive not registered in VFS | Register `t:` to `\Device\Harddisk0\Partition1` |
| Crash log lost on `abort()` | SIGABRT stack-trace handler |

### Timing and performance

| Problem | Fix |
|---|---|
| 2x game speed above 30 fps | `MCLAUseRealDelta` at `0x821BDB58` and `MCLAFixedStepPath` at `0x821BDB90`. There are **two** fixed-timestep paths; patching only one leaves the game at 2x |
| Accumulated-time totals frozen | Hook placement narrowed so `[r3+20]`/`[r3+24]` keep advancing |
| Frame times quantized to a 15.625 ms grid | `timeBeginPeriod(1)` **and** `vsync=false`. Either alone leaves the grid intact. ~30% throughput gain |
| Nothing throttling presentation once vsync was off | Time-based frame limiter (`MCLA_FPS_CAP`) |
| Physics exploding after a streaming stall | Per-frame delta clamp (`MCLA_MAX_FRAME_MS`, default 125 ms) |
| Camera/suspension behaving differently at high fps | Continuous-time exponential decay calibrated to the 30 FPS curve |
| Millions of emulated `dcbf`/`dcbst` iterations/sec | `mc_FlushDataCache` bypass with a retained memory fence |
| Entire GPU config silently ignored | GPU cvars moved to `OnPostSetup`, where the plugin is actually loaded |
| ~7,500 log lines/sec during gameplay | `REX_LOG_LEVEL=warn` |

For the engine-level detail behind these, see
[`TECHNICAL_NOTES.md`](TECHNICAL_NOTES.md).
