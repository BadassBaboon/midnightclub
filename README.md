# Midnight Club: Los Angeles — Static Recompilation

A static recompilation of **Midnight Club: Los Angeles (Complete Edition)** for Xbox 360, targeting Windows x86-64. Built with the [rexGlu SDK](https://github.com/rexglue/rexglue) as a compilers class project.

Static recompilation converts the original Xbox 360 PowerPC (PPC) bytecode in the game's `.xex` executable into native C++ that compiles and runs directly on a modern PC — no emulator, no interpreter. The game's kernel calls (file I/O, GPU commands, audio, threading) are handled by the rexGlu runtime.

---

## What Works

- Game boots and reaches the main menu
- City renders with traffic
- Save profile creation works
- Free roam / race selection is reachable

## Known Issues

- Car falls through the road (road collision mesh not yet loading correctly)

---

## Prerequisites

| Requirement | Notes |
|---|---|
| **rexGlu SDK v0.7.8.2-dev** | Must be installed and on PATH (`rexglue` command available) |
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

- `midnightclub_config.toml` → `file_path` (path to `default.xex`)
- `src/midnightclub_app.h` → `paths.game_data_root` (path to the folder containing `default.xex`)

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

---

## If You Re-Run Codegen

`rexglue codegen midnightclub_config.toml` regenerates the `generated/` directory and will **overwrite the `mullhwu.` fix**. After regenerating, find this in `generated/midnightclub_recomp.43.cpp`:

```cpp
// mullhwu. r19,r0,r28
// UNIMPLEMENTED: mullhwu.
REX_UNIMPLEMENTED(0x825E18C4, "mullhwu.");
```

Replace it with:

```cpp
// mullhwu. r19,r0,r28
ctx.r19.u64 = uint64_t((uint64_t(ctx.r0.u32) * uint64_t(ctx.r28.u32)) >> 32);
ctx.cr0.compare<int32_t>(ctx.r19.s32, 0, ctx.xer);
```

---

## How It Works / Fixes Applied

The rexGlu SDK translates each PPC function in the XEX into a C++ function. Functions not in the hint list get a no-op stub registered at runtime. Several things needed fixing before the game would run:

| Problem | Fix |
|---|---|
| Static initializers calling unregistered functions | Pass 1: scan the XEX init tables and stub any missing entries |
| "Dirty Disc" error shown for extracted files | Bypass `sub_82130678` (the disc-error handler) with a no-op |
| Indirect calls to any unregistered code address | Pass 2: stub the entire code region `[0x82130000, 0x827CD054]` |
| `mullhwu.` instruction not implemented by rexGlu | Implemented manually in `generated/midnightclub_recomp.43.cpp` — this was crashing on save creation |
| `t:` drive not registered in VFS | Register `t:` → `\Device\Harddisk0\Partition1` so city art/data lookups resolve |
| Crash log lost when `abort()` is called | SIGABRT handler in `OnPostSetup` writes a stack trace to `crash_stack.txt` before dying |

### Debugging Aids

- **`stubs.txt`** — written to the repo root at runtime. Every call to a stubbed PPC address logs `addr` + `LR`. The last entry before a crash tells you which unimplemented function was called.
- **`crash_stack.txt`** — written on `abort()`. Full call stack from the crash site. Check here first when the game dies.

---

## Project Structure

```
midnightclub/
  src/
    main.cpp              # Entry point (rexGlu-generated, do not edit)
    midnightclub_app.h    # App customization: paths, stubs, VFS setup
  generated/
    midnightclub_init.h   # Runtime macros and REX_CALL_INDIRECT_FUNC definition
    midnightclub_recomp.*.cpp  # PPC → C++ translated game code (60 files)
    rexglue.cmake         # SDK CMake integration
  midnightclub_config.toml  # rexGlu codegen config (function hints, XEX path)
  CMakeLists.txt
  CMakePresets.json
  launch.vs.json          # Visual Studio debug launch config
  RUNNING.md              # Quick-reference run guide
```
