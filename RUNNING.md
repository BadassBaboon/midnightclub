# How to Run Midnight Club LA (Recompiled)

## Quick Start

```powershell
cd C:\Users\zarif\Documents\repo\midnightclub\out\build\win-amd64-relwithdebinfo
.\midnightclub.exe
```

Or use Visual Studio: open the repo folder, select the **Windows AMD64 RelWithDebInfo** configuration from the dropdown, and click Play.

---

## Build from Scratch

```powershell
cd C:\Users\zarif\Documents\repo\midnightclub
cmake --build out/build/win-amd64-relwithdebinfo --target midnightclub
```

If you need to reconfigure first:
```powershell
cmake --preset win-amd64-relwithdebinfo .
cmake --build out/build/win-amd64-relwithdebinfo --target midnightclub
```

---

## If You Re-Run `rexglue codegen`

The codegen regenerates `generated/` and will **overwrite** the `mullhwu.` fix. After re-running codegen, patch it back manually:

In `generated/midnightclub_recomp.43.cpp` around the line containing `mullhwu.`, replace:

```cpp
// mullhwu. r19,r0,r28
// UNIMPLEMENTED: mullhwu.
REX_UNIMPLEMENTED(0x825E18C4, "mullhwu.");
```

with:

```cpp
// mullhwu. r19,r0,r28
ctx.r19.u64 = uint64_t((uint64_t(ctx.r0.u32) * uint64_t(ctx.r28.u32)) >> 32);
ctx.cr0.compare<int32_t>(ctx.r19.s32, 0, ctx.xer);
```

---

## Save Data

- Stored at: `C:\Users\zarif\Documents\midnightclub\`
- On first run the game asks you to create a save profile — do it once and you're done.
- To reset to first-run state: delete `C:\Users\zarif\Documents\midnightclub\B13EBABEBABEBABE\`

---

## Debugging Crashes

- `stubs.txt` in the repo root: logs every stubbed PPC function call (addr + LR). Last entry before a crash = what was called.
- `crash_stack.txt` in the repo root: written on `abort()` — full call stack from the crash site. Look here first when the game dies.

---

## What Was Fixed (summary)

| Problem | Fix |
|---|---|
| Static initializers missing functions | Pass 1: scan init tables, stub missing entries |
| "Dirty Disc" error on extracted files | Bypass `sub_82130678` with no-op |
| Indirect calls to unregistered addresses | Pass 2: stub entire code region [0x82130000–0x827CD054] |
| `mullhwu.` unimplemented instruction → crash during save | Implemented manually in `midnightclub_recomp.43.cpp` |
| Crash log lost on abort() | File-backed stubs.txt + SIGABRT stack trace handler |
