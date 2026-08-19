# Midnight Club: Los Angeles - Recompilation Technical Notes

Findings from getting MCLA (Xbox 360, Title ID `545407F8`) running as a static
recompilation on Windows x86-64 via **ReXGlue 0.9.0**, with the emphasis on the
frame-timing work.

Everything below was verified by measurement or by reading the disassembly.
Where something is inferred rather than proven, it says so. The full
investigation log - including hypotheses that turned out to be wrong - is in
[`MCLA_workplan.md`](MCLA_workplan.md).

> This file replaces an earlier `recompilation_technical_rundown.md`, written
> mid-investigation, which stated several things later testing disproved.
> Corrections are called out inline so anyone who read the old version can see
> exactly what changed.

---

## 1. The engine frame timer: `sub_821BDA90`

The timer object lives at a fixed address, so its field offsets are absolute:

| offset | address | meaning |
|--------|---------|---------|
| +0x00  | `0x827D7500` | vtable |
| +0x08  | `0x827D7508` | frame delta, time-scaled - what the engine consumes |
| +0x0C  | `0x827D750C` | `1.0 / delta` |
| +0x14  | `0x827D7514` | accumulated time A |
| +0x18  | `0x827D7518` | accumulated time B |
| +0x38  | `0x827D7538` | one-shot "timer was reset" flag |
| +0x40  | `0x827D7540` | last timebase tick (u64) |
| +0x54  | `0x827D7554` | time scale (1.0) |
| +0x58  | `0x827D7558` | frame delta, unscaled |

Constants:

| symbol | value | meaning |
|--------|-------|---------|
| `flt_82011110` | 2.00504e-8 | tick -> seconds |
| `flt_82001D14` | 1.0 | numerator of `1.0 / target_fps` |
| `flt_82000ED4` | 0.0 | zero compare |
| `flt_82003770` | -1.0 | passed as `f1` = "no forced delta, use measured" |

**CORRECTION:** the guest timebase is **49,875,000 Hz**, not 50,000,000. The
earlier document said 50 MHz. Derived from `flt_82011110` = 2.00504e-8, whose
reciprocal is 49,874,300. Query `Clock::guest_tick_frequency()` rather than
hardcoding either number.

---

## 2. Why the Xenia 60 FPS patch is not enough

The community Xenia patch is:

```toml
address = 0x821bdb08
value   = 0x4800012c   # b 0x821bdc34
address = 0x82419aa3
value   = 0x01
```

**CORRECTION:** `0x821BDB08` is `bne cr6, 0x821BDBC8`, **not** `fcmpu`. An
earlier write-up placed the patch at `0x821BDB68` on that misreading, which left
the main fixed-timestep path live.

**CORRECTION:** `0x82419AA0` (`li r11, 2`) is **not** a delta-time value. It is
a **PM4 present-interval field** - `sub_824199B0` builds GPU command packets and
shifts `r11` into a packet field at `0x82419AB4`. It sets how many vblanks to
wait between presents (2 = 30 Hz), nothing more.

There are **two** fixed-timestep paths. The Xenia patch's blanket jump happens
to skip both; patching only one leaves the game at 2x speed.

- `loc_821BDB58` - compares measured delta against the fixed step, overwrites
  `[r3+8]`/`[r3+88]`, or sets a "skip frame" flag at `[r3+52]`.
- `loc_821BDB90` - taken when `[r3+58]` or `[r3+60]` are set. **Measured: taken
  on EVERY frame during gameplay**, not occasionally. Overwrites `[r3+8]` with
  the fixed 33.3 ms step regardless of real elapsed time.

The blanket jump also skips the block advancing `[r3+20]` and `[r3+24]`, the
engine's accumulated-time totals. Measured: they advanced exactly once and then
sat frozen at 0.0333 for an entire session.

### What actually works

Three mid-asm hooks, not one byte patch:

| hook | address | purpose |
|------|---------|---------|
| `MCLAUseRealDelta` | `0x821BDB58` | jump to `loc_821BDC34`, skipping only the fixed-step overwrite. The `[r3+56]` reset guard at `0x821BDB08` still runs |
| `MCLAFixedStepPath` | `0x821BDB90` | replace the freshly-loaded fixed step in `f11` with the measured unscaled delta from `[r3+88]`. Downstream instructions then do the right thing unaided, and the accumulators keep working |
| `MCLAFrameDelta` | `0x821BDAB0` | clamp the per-frame delta so a streaming stall cannot feed an unbounded value into the physics and audio clocks |

Keeping the `[r3+56]` reset guard matters: on a reset frame the last-tick field
is stale, so the measured delta is garbage. Xenia's patch skips that guard
unconditionally. Doing the same here, with the clamp removed, produced a very
loud audio blowout.

**Verification method:** sum the delta actually used across every substep-loop
pass and compare against real time. It reads 2.00x at a 30 fps cap and 2.00x at
60 - the 2.00 is a metric artifact (three passes per frame: two of `dt/2` plus
one full `dt`). What matters is that the number does not change with frame rate.

---

## 3. Frame pacing: the stutter was Windows timer granularity

Frame times were quantized to a **15.625 ms grid = 1/64 s**, with the buckets
between clusters completely empty - not sparse, empty.

Not the display (a 170 Hz panel is 5.88 ms), not 60 Hz vsync (16.67 ms), and not
the guest vblank rate - sweeping `video_mode_refresh_rate` across 30/60/120/144
did not move the grid at all.

| timer resolution | vsync | frames on-grid | fps |
|---|---|---|---|
| default (~15.6 ms) | on | 95% | 37.2 |
| 1 ms | on | 62% | 40.6 (regrids to true 60 Hz) |
| default | off | 93% | 40.7 (grid survives - vsync was never the cause) |
| **1 ms** | **off** | **34%** | **48.4** |

**Both changes are required.** `timeBeginPeriod(1)` alone hands pacing to real
vsync; disabling vsync alone leaves the timer grid intact. Together the
distribution goes continuous and throughput rises ~30%.

Quantization costs frame rate, not just smoothness: a frame overrunning its
quantum by 1 ms waits a full extra 15.6 ms.

Removing vsync also removes the only throttle, so a **time-based** frame limiter
is needed - sleeping to a wall-clock deadline. A vblank-based limiter would
reintroduce the grid. The guest's present-interval field cannot do this job: it
only means anything to DXGI when vsync is on.

---

## 4. rexglue-specific gotchas

- **GPU cvars cannot be set from `OnPreSetup`.** Everything in
  `rex/graphics/flags.h` lives in the GPU plugin DLL, which `Runtime::Setup()`
  loads *after* `OnPreSetup` returns. Such calls report `FAIL` there and `ok`
  from `OnPostSetup`. This silently discarded an entire GPU configuration - most
  consequentially `vsync=false`, which stayed at its default of `true`.
  `SetFlagByName` returns `false` in that case; check the return value.
- **Log level defaults to `trace` on non-Release builds** - ~7,500 lines/sec and
  ~1.4 MB/s of synchronous disk I/O during gameplay. A genuine stutter source.
  Set `REX_LOG_LEVEL=warn`. Setting it from `OnPostInitLogging()` does not work.
- **`anisotropic_override` is an enum index**, not a multiplier:
  `-1` none, `0` off, `1` 1x, `2` 2x, `3` 4x, `4` 8x, **`5` 16x**.
- **`mount_cache` is not a cvar** in 0.9.0, despite appearing in community
  configs. **CORRECTION:** an earlier document claimed RPF RAM caching was
  enabled through it. It never was.
- **`clear_memory_page_state=false`** breaks memory coherency and makes the
  render-to-texture minimap flicker white. Leave it at the default.
- **`resolution_scale=2` is broken** for this title: it corrupts the projection
  aspect feeding frustum-cull plane normals, so race-start showcase cameras pick
  opponents behind the camera, and grid spawn transforms get garbage rotation
  (cars spawn tilted, floating). Leave at 1; the window is upscaled anyway.
- **Mid-asm hook signatures** take only the registers named in the hook's
  `registers` list, by reference, with ordinary C++ linkage - not `extern "C"`,
  not `(ctx, base)`. Float registers arrive as `PPCRegister&` with the value in
  `.f64`. Returning `bool` drives `jump_address_on_true`.
- **Perf counters** (`rex/perf/counter.h`) are exported and callable even though
  the CSV writer is compiled out of the shipped DLL. Live: `buffer_queue_depth`,
  `texture_cache_hits/misses`, `draw_calls`, `active_threads`. Always zero in
  the shipped build: `xma_frames_decoded`, `audio_frame_latency_us`,
  `command_buffer_stalls`, `critical_region_contentions`, `apc_queue_depth`.

---

## 5. Performance findings

- **`mc_FlushDataCache` (`0x821D5510`)** is a `dcbf`/`dcbst` loop walking a
  buffer 128 bytes at a time. On x86 host caches are already coherent, so it can
  be replaced with an immediate return plus a memory fence. Measured over an
  8.5 minute session: median **4,976 calls/sec** (peak 9,936), mean **66 MB/s**
  of flush range (peak 229 MB/s), **33.5 GB** total. That is roughly **540,000
  avoided 128-byte line operations per second**, peaking near 1.9 million.
  Single largest CPU saving in the project.
  Keep the fence: the loop is also a publication point, and the XMA decoder and
  audio worker run on their own host threads.
- **Texture cache**: raising limits to 1536 MB soft / 2048 MB hard / 64 MB
  render-to-texture takes `texture_cache_misses` to **zero**. Avenue closed.
- **Draw calls run ~3,400-3,700/sec** at ~50 fps - about 70 per frame, which is
  low. Dense-area slowdowns are therefore **not** draw-call submission; the cost
  is in the recompiled guest code (traffic AI, physics, streaming).
- The stub safety net registers ~1.7 million unmapped addresses in ~400-500 ms
  at startup. `stubs.txt` stays empty in practice, but the net converts an
  unmapped indirect call from a crash into a logged no-op, so it stays on.

---

## 6. Known limitations

- **Intro BIK/legal movies play fast.** Inherent to the 60 FPS unlock; LARecomp
  shows the same behaviour with the same method. Workarounds:
  `MCLA_SKIP_INTRO=1`, `MCLA_VSYNC=true`, or press A. Not treated as a bug.
- **Car reflections, dithered alpha, occasional HUD glitches** come from
  rexglue's `xenos` plugin lacking rendering fixes that
  [xenia-edge](https://github.com/has207/xenia-edge) carries. The plugin ships
  as a prebuilt DLL with no source, so those fixes cannot be ported.
- **Audio jitter in dense areas**, worst in the cockpit camera and on tyre skid
  loops. Four causes eliminated by measurement: memory ordering, general CPU
  starvation, output buffer depth (`audio_maxqframes` 8/16/32), and output
  underrun (`buffer_queue_depth` stays pinned at maximum and never drains).
  What remains is per-voice DSP or mixing inside the guest audio engine.
  Unresolved.

---

## 7. Plausible-sounding things that are wrong

Recorded so nobody re-tests them:

| claim | reality |
|---|---|
| The 60 FPS patch belongs at `0x821BDB68` | It belongs at `0x821BDB08`/`0x821BDB58`. The earlier address came from misreading `bne` as `fcmpu` |
| `0x82419AA0` is a delta-time patch | It is a PM4 present-interval field |
| The guest timebase is 50 MHz | 49.875 MHz |
| `mount_cache` enables RPF RAM caching | Not a registered cvar in 0.9.0 |
| Dummy `.loc` files remove streaming overhead | They are leftover `test_`-prefixed dev assets. Exactly 7 warnings fire once per session at startup and never recur - there is no per-frame cost to remove. Fabricating empty files risks the parser accepting garbage instead of cleanly failing |
| The stutter is vsync | It was Windows timer granularity. Vsync alone leaves 93% of frames on-grid |
| Physics substeps are fixed per frame | `sub_821BD910` divides `dt` by the substep count correctly |
| Camera smoothing can be fixed at the shared lerp `sub_8231D3A8` | That lerp is shared with cockpit view, wheel animation, speedometer and HUD; hooking it breaks all of them. Hook the specific caller instead |
| Grepping for `0xF0(rN)` proves a field is unused | Fields passed **by address** to a helper never appear as a displacement. `mcDofObject::coc_vector` is uploaded via `addi r29, r31, 0xF0` -> shader parameter setter |
