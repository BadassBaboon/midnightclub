# How to Run Midnight Club LA (Recompiled)

## Quick Start - recommended

```powershell
cd out\build\win-amd64-relwithdebinfo
$env:REX_LOG_LEVEL="warn"; $env:MCLA_FPS_CAP="30"; Start-Process midnightclub.exe
```

**30 fps is fully correct and is the recommended way to play.** Physics,
steering, collisions, camera and traffic all behave as on a real console, and
frame pacing is smooth and continuous.

This is not the old 30 fps lock. The original game reached 30 fps by waiting on
vblank, which quantized frame times to a 15.625 ms grid and caused constant
micro-stutter. The cap here is a time-based limiter, so frame times are evenly
spaced.

Or use Visual Studio: open the repo folder, select the **Windows AMD64
RelWithDebInfo** configuration from the dropdown, and click Play. Note that this
path does not set the environment variables above - you will get uncapped frame
rate and trace-level logging.

## 60 fps

```powershell
$env:REX_LOG_LEVEL="warn"; $env:MCLA_FPS_CAP="60"; Start-Process midnightclub.exe
```

Runs and is stable, and the simulation advances at the correct speed (verified
by measurement). The camera and traffic still *feel* faster because some
subsystems smooth their motion by a per-frame constant rather than by elapsed
time. Playable, but 30 fps is more faithful today.

Leave `MCLA_FPS_CAP` unset to run uncapped - not recommended, the game reaches
70-100+ fps in light scenes.

**Always set `REX_LOG_LEVEL=warn`.** Non-Release builds default to `trace`,
which costs ~7,500 log lines/sec and ~1.4 MB/s of synchronous disk I/O during
gameplay. It is a real source of stutter, not just noise.

See the README for the full environment-variable table.

---

## Build from Scratch

```powershell
cmake --build out/build/win-amd64-relwithdebinfo --target midnightclub
```

If you need to reconfigure first:

```powershell
cmake --preset win-amd64-relwithdebinfo .
cmake --build out/build/win-amd64-relwithdebinfo --target midnightclub
```

---

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

---

## Save Data

- Stored under `user_data\` in the build directory.
- On first run the game asks you to create a save profile - do it once.
- To reset to first-run state, delete the profile folder under `user_data\`.

---

## Debugging

- **`logs\effective_config.txt`** - the *actual* value of every cvar we care
  about, plus an `ok`/`FAIL` line per `SetFlagByName` attempt. Check this first
  when a setting seems to have no effect. GPU cvars only bind from
  `OnPostSetup`, never from `OnPreSetup`.
- **`logs\timing_<date>_<time>_cap<N>.log`** - with `MCLA_TIMING_LOG=1`.
  Per-second frame rate and spike counts, plus a 1 ms frame-time histogram every
  10 s. One file per run, so back-to-back runs do not overwrite each other.
- **`stubs.txt`** - every stubbed PPC function call (addr + LR). Last entry
  before a crash is what was called.
- **`crash_stack.txt`** - written on `abort()`, full call stack from the crash.

---

## What Was Fixed (summary)

### Getting it to boot

| Problem | Fix |
|---|---|
| Static initializers missing functions | Pass 1: scan init tables, stub missing entries |
| "Dirty Disc" error on extracted files | Bypass `sub_82130678` with a no-op |
| Indirect calls to unregistered addresses | Pass 2: stub entire code region [0x82130000-0x827CD054] |
| `t:` drive not registered in VFS | Register `t:` - `\Device\Harddisk0\Partition1` |
| Crash log lost on `abort()` | File-backed `stubs.txt` + SIGABRT stack trace handler |

### Timing and performance

| Problem | Fix |
|---|---|
| 2x game speed above 30 fps | `MCLAUseRealDelta` hook at `0x821BDB08` - the engine had a fixed-timestep path that discarded the measured delta |
| Present interval locked to 30 Hz | `MCLAPresentInterval` hook at `0x82419AA0` (a PM4 packet field), `2` - `1` vblanks |
| Frame times quantized to a 15.625 ms grid | `timeBeginPeriod(1)` **and** `vsync=false` - the grid was Windows' default timer granularity; either fix alone leaves it intact. ~30% throughput gain |
| Nothing throttling presentation once vsync was off | Time-based frame limiter (`MCLA_FPS_CAP`) |
| Physics exploding after a streaming stall | Per-frame delta clamp (`MCLA_MAX_FRAME_MS`, default 125 ms) |
| Entire GPU config silently ignored | GPU cvars live in the xenos plugin DLL, which loads after `OnPreSetup` - moved to `OnPostSetup` |
| ~7,500 log lines/sec during gameplay | `REX_LOG_LEVEL=warn` |
