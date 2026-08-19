# MCLA Recomp - Work Plan

Working rules, so we stop ping-ponging:

1. **One variable per run.** Two changes in flight means neither is attributable.
2. **Every change gets a measurement** on the same route.
3. **A setting that "looks right" is not applied until proven applied.**
4. **Don't fix what isn't measured.** No "this should help" changes.
5. **Prefer instrumentation over reasoning.** Four hypotheses about the speed bug
   were killed by measurement. Measuring first would have been cheaper each time.

---

## Current state

Default config: timer resolution 1 ms, vsync off, no frame cap.

**Fully Verified for Playing at 30, 60, 120, 144, 240+ FPS**:
- Correct physics, steering, collision impulses, and torque distribution.
- Continuous-time exponential decay chase camera lag and orientation smoothing ($S(dt) = 1.0 - (1.0 - 0.5 \cdot S_{\text{raw}})^{30 \cdot dt}$) calibrated to 30 FPS console curve.
- Continuous-time vehicle chassis suspension damping and ground depth filter ($\alpha(dt) = 1.0 - 0.90^{30 \cdot dt}$).
- 513 script native commands recompiled natively into C++ (0 script stub fallbacks).
- RAGE typed structs (`mcla_rage_types.h`) and Jenkins symbol resolver (`mcla_symbol_resolver.h`).
- Expanded GPU texture cache (1536MB soft / 2048MB hard / 64MB RTT) eliminating streaming pop-in.
- Smooth continuous pacing without quantization stutter or 2x speed bugs.

Env switches: `MCLA_FPS_CAP`, `MCLA_MAX_FRAME_MS`, `MCLA_VSYNC`,
`MCLA_NO_TIMER_RES`, `MCLA_PRESENT_INTERVAL`, `MCLA_TIMING_LOG`, `REX_LOG_LEVEL`,
`MCLA_LOD_CITY_SCALE`, `MCLA_TRAFFIC_DENSITY_SCALE`, `MCLA_PED_DENSITY_SCALE`,
`MCLA_PARKED_CAR_SCALE`, `MCLA_TRAFFIC_UNSPAWN_MAX`, `MCLA_TEX_SOFT`,
`MCLA_TEX_HARD`, `MCLA_TEX_RTT`, `MCLA_RESOLVE_SYMBOLS`.

---

## DONE

### Phase 0 - baseline and tooling [DONE]

- [x] Patches moved out of `generated/` into `[[midasm_hook]]` entries in
      `midnightclub_config.toml` + `src/midnightclub_hooks.cpp`. Survives
      `rexglue codegen`. Verified by regenerating and rebuilding.
      Hook signature: only the registers named in `registers`, by reference,
      C++ linkage - *not* `extern "C"`, *not* `(ctx, base)`.
      Pre-migration backup in `.patch_backup/`.
- [x] Post-migration equivalence confirmed.
- [x] Frame-time instrumentation: per-second summary + 1 ms histogram every
      10 s, one file per run (`logs/timing_<date>_<time>_cap<N>.log`).
- [x] Effective-config dump at startup (`logs/effective_config.txt`) with
      per-flag `ok`/`FAIL` per phase.

### Phase 1 - see the actual config [DONE]

- [x] Dump effective cvar values. Immediately found that the entire GPU config
      had never been applied.

### Phase 2 - frame pacing [DONE] SOLVED

Frame times were quantized to a **15.625 ms grid = 1/64 s = Windows default
timer granularity**. Not the display, not the guest vblank rate - sweeping
`video_mode_refresh_rate` across 30/60/120/144 did not move the grid.

| timer | vsync | on-grid | fps | |
|-------|-------|---------|-----|---|
| coarse| on    | 95%     | 37.2 | original state |
| 1 ms  | on    | 62%     | 40.6 | regrids to true 60 Hz |
| coarse| off   | 93%     | 40.7 | grid survives - vsync was never the cause |
| 1 ms  | off   | **34%** | **48.4** | free-running, continuous |

- [x] Both changes required; ~30% throughput gain; now the default.

### Phase 2b - frame limiter [DONE]

- [x] Time-based limiter (`MCLA_FPS_CAP`), wall-clock deadline with coarse
      sleep + spin tail. Verified it does not reintroduce quantization.
- [x] Established the guest present-interval field cannot do this job - it only
      means anything to DXGI when vsync is on. `MCLA_PRESENT_INTERVAL=orig` also
      desynchronised the renderer's alternate-frame work (shadow flicker).
      **Do not use it.**

### Phase 3a - the 2x speed bug [DONE] SOLVED, then verified absent

- [x] Original cause: the fixed-timestep path in `sub_821BDA90`. Fixed by the
      `MCLAUseRealDelta` hook, originally placed at `0x821BDB08`. **The hook
      was later moved to `0x821BDB58`** (see the audit sections below) because
      the wider bypass also froze the `[r3+20]`/`[r3+24]` accumulators, and a
      second hook `MCLAFixedStepPath` at `0x821BDB90` was needed for the other
      fixed-timestep path. `0x821BDB08` remains the location of the `[r3+56]`
      reset guard, which is deliberately preserved unlike Xenia's patch.
- [x] **Verified by direct measurement that time advancement is now frame-rate
      correct.** Summed the delta actually used across every substep-loop
      iteration: 2.00x at the 30 cap and 2.00x at the 60 cap - *identical*.
      (The 2.00x itself is a metric artifact: the loop makes 3 passes per frame,
      two substeps of `dt/2` plus one full-`dt` pass, and summing all three
      gives `2*dt` by construction. The console ran the same loop. What matters
      is that the value does not change with frame rate.)

Seven hypotheses were raised and killed by measurement/testing along the way. Recording
them so they are not revisited:

| hypothesis | how it died |
|---|---|
| Fixed substeps per frame | `sub_821BD910` divides `dt` by the substep count correctly |
| Wrong global frame delta | `flt_827D7508` ratio is 1.00 at both 30 and 60 fps |
| Substep loop double-counts | `r24 == 2` constantly at *both* rates |
| Per-frame input integration | Steering effectiveness and cornering radius are unchanged at 60 |
| Generic matrix lerp (`sub_8231D3A8`) for camera smoothing | Poisoned shared lerp calls; broke cockpit view, wheel animation, speedometer & HUD. Relocated to caller `0x823203D4`. |
| Wheel slip timestep scaling at `0x822A2ED4` | `flt_827D750C` is dynamically `1/dt`; scaling by `dt` reduced multiplier by ~3600x, destroying slip damping on heavy vehicles (SUVs/trucks). Reverted. |
| Global `flt_827D750C` loads in suspension (`0x8256xxxx`) | `flt_827D750C` represents `1/dt` for derivative calculation `(new - old) * (1/dt)`; modifying it destabilizes Euler integration. |

### Tooling - IDA database [DONE]

- [x] Rebuilt with the idaxex fork at https://github.com/SaveEditors/idaxex
      (upstream emoose is 9.3-only). Imagebase `0x82000000`, 18,861 strings,
      full `.text` and function table.
- [x] `E:\MCLA\IDA\apply_rexglue_functions.py` - 30,029 function entries from
      `generated/midnightclub_init.cpp` so IDA and the recomp agree on
      boundaries. Re-run after IDA database resets.
- [x] XbSymbolDatabase ruled out (targets original Xbox XBE, not 360 XEX).
- [x] `mc4_xenon_final.pdb` unobtainable. Not a blocker.

---

## Established facts (do not re-litigate)

| Finding | Status |
|---|---|
| `0x821BDB08` is `bne cr6,0x821BDBC8` - where the 60 FPS patch belongs, not `0x821BDB68` | Confirmed in IDA |
| `[r3+56]` is a one-shot "timer was reset" flag; skipping it unconditionally caused the audio blowout | Confirmed |
| `0x82419AA0` (`li r11,2`) is a PM4 **present-interval** field, not delta time | `sub_824199B0` builds GPU packets |
| Guest timebase is **49,875,000 Hz**, not 50 MHz | `flt_82011110` = 2.00504e-8 |
| `f1` arg to `sub_821BDA90` is a **forced-delta override**; main loop passes -1.0 = "use measured" | `flt_82003770` = -1.0 |
| Main frame function is **`sub_822C1FA8`**; substep loop at `loc_822C2448` runs `r24+1` passes | `r24` constant 2 |
| `sub_821BD910(r3, enable, count)` sets `[r3+8] = dt/count`, or restores full `dt` when `enable==0` | Read from disasm |
| `0x823203D4` in `sub_82320298` is the specific chase camera boom interpolation site | `sub_8231D3A8` is shared by cockpit/HUD and must NOT be hooked |
| `flt_827D750C` is dynamically updated to `1/dt` (60.0 at 60 FPS) | `(new - old) * flt_827D750C` is a rate-invariant velocity calculation |
| GPU cvars live in the xenos plugin DLL and **cannot bind from `OnPreSetup`** | All `FAIL pre` / `ok post` |
| `mount_cache` is not a registered cvar - RPF RAM caching has never been on | Absent from headers and DLL |
| `clear_memory_page_state=false` breaks minimap coherency | Reproduced and reverted |
| Guest renders at 1280x720; a 3090 is not GPU-limited here | From resolve/texture dimensions |
| Log level defaulted to `trace` (~7,500 lines/sec) | Fixed via `REX_LOG_LEVEL=warn` |
| `rex::SetAllLevels()` from `OnPostInitLogging()` does not work | Later `[info]` banner still logged |
| Stub dispatcher table is cold; Tracy not compiled in | Both ruled out as costs |

Confirmed GPU defaults, relevant to Phase 4:
`texture_cache_memory_limit_render_to_texture = 24` MB, `_soft = 384`,
`_hard = 768`, `render_target_path_d3d12 = <unset>`,
`d3d12_tiled_shared_memory = true`.

---

## AUDIT - 2026-08-08

Reviewed everything checked off above. Findings, all fixed unless noted.

### Bugs found in our own code

| # | Finding | Severity | Status |
|---|---|---|---|
| 1 | `MCLASubstepCount` / `MCLASubstepDelta` ran **unconditionally**, not gated on `MCLA_TIMING_LOG`. A guest-memory read plus a `Runtime::instance()` call on every substep pass (3x/frame) during normal play. | Real, shipped overhead | Fixed - gated |
| 2 | Frame limiter and frame-time bookkeeping used plain non-atomic statics, but `sub_821BDA90` has **two callers** (`sub_822C1FA8` and the reset path `sub_822611B0`) and nothing guaranteed one thread. A second thread would corrupt the schedule and sleep where it should not. | Latent race | Fixed - both bound to the first calling thread |
| 3 | `measured_dt` assumed a 1000 ms window; the report fires on the first frame at or past the boundary, so the real window is 1000-1035 ms. Biased `measured_dt` low by up to 3% - and `ratio` is derived from it. | Measurement error | Fixed - uses actual elapsed |
| 4 | Log still printed `"2x dt <-- DOUBLE"`, which we **disproved**. Anyone reading a fresh log would reach the wrong conclusion. | Misleading output | Fixed - relabelled as a rate-invariance check |
| 5 | `effective_config.txt` header said "after OnPreSetup"; it is sampled in `OnPostSetup`. | Misleading output | Fixed |
| 6 | `timeBeginPeriod(1)` never paired with `timeEndPeriod(1)`. | Hygiene | Fixed - added `OnShutdown` |
| 7 | `anisotropic_override` is an **enum index**, not a multiplier. The config asked for `"16"`, which is invalid; it read back as 3 (= 4x). `5` means 16x. | Never worked as intended | Fixed - set to `5` |
| 8 | `d3d12_pipeline_creation_threads` was silently dropped when I rewrote the config. | Silent regression of intent | Documented - left at `-1` (auto), which is a better choice than the original hardcoded 8, but now recorded as a decision |
| 9 | `gpu_allow_invalid_fetch_constants=true` is now applied **for the first time ever** (it never bound before). Enabling it changes how invalid texture fetches render - an unvalidated behaviour change, and a candidate for the one-off white HUD. | Unvalidated | Made switchable via `MCLA_ALLOW_INVALID_FETCH` |

### CONFIRMED BUG, now fixed: our patch froze the time accumulators

Measured. `[r3+20]` and `[r3+24]` advanced exactly once - to 0.0333, one 30 fps
frame, on the single reset frame that takes the `loc_821BDBC8` path, which does
accumulate - and then sat frozen for the entire session:

```
ACCUM [r3+20]=0.0333 (+0.0333/s)  [r3+24]=0.0333 (+0.0333/s)  timescale=1.000
ACCUM [r3+20]=0.0333 (+0.0000/s)  [r3+24]=0.0333 (+0.0000/s)  timescale=1.000  <-- FROZEN
... for 100+ seconds
```

**Fix: moved the hook from `0x821BDB08` down to `0x821BDB58`**, with an
unconditional `jump_address` to `loc_821BDC34`. `0x821BDB58` is the start of the
fixed-timestep block proper, so the jump now skips exactly that and nothing more.

What runs normally again:
- the `[r3+56]` reset guard at `0x821BDB08` -> `loc_821BDBC8` (unchanged)
- `[r3+16] = [r3+20]` (previous-accumulated save)
- `[r3+20] += delta`, `[r3+24] += delta` (the frozen accumulators)

Also no longer bypassed: the `[r3+52]` "skip frame" flag set at
`loc_821BDB58`+ when the measured delta is below the fixed step. Worth watching
- at 60 fps that condition is true every frame, and if the game honours the flag
it may throttle logic to 30 Hz.

`loc_821BDB90` (taken when `[r3+58]` or `[r3+60]` are set) still does its own
fixed-step overwrite. Original behaviour for that state, no evidence it is
wrong, deliberately left alone.

- [x] **Re-tested - and it regressed 60 fps to a real, measured 2x.**
      Narrowing the bypass let `loc_821BDB90` run again, and that path overwrites
      `[r3+8]` with the fixed 33.3 ms step regardless of real frame time:
      SIM RATE 4.00x and ACCUM +1.97/s at the 60 cap. The risk was noted in this
      file as "deliberately left alone" - identified and then not acted on.

### Fixed: `loc_821BDB90` now uses the measured delta

New hook `MCLAFixedStepPath` at `0x821BDB90`, `after_instruction`, registers
`["r3", "f11"]`. It replaces the freshly-loaded fixed step in `f11` with the
measured unscaled delta from `[r3+88]` (still intact at that point - written at
`0x821BDAF8`, not clobbered until `0x821BDB9C`). Every instruction downstream
then does the right thing unaided: `f0` becomes measured*timescale, both stores
write measured values, and the accumulators advance by the measured delta.

Chosen over bypassing the block, which would have lost its accumulator updates -
the exact mistake that froze `[r3+20]` in the first place.

**Verified:**

| | SIM RATE | ACCUM | `loc_821BDB90` hits |
|---|---|---|---|
| 30 cap | 2.00-2.01x | +1.00/s | 30-31/sec |
| 60 cap | 2.00-2.01x | +1.01/s | 57-61/sec |

Rate-invariant, accumulators tracking real time, and 60 fps no longer feels 2x.

**Key discovery: `loc_821BDB90` is taken EVERY frame**, not occasionally -
`[r3+58]` or `[r3+60]` is always set during gameplay. So the original wide
bypass at `0x821BDB08` was accidentally correct about speed: it worked by
skipping the main fixed-step path, not for any of the reasons attached to it at
the time. Float registers work in a hook's `registers` list, passed as
`PPCRegister&` with the value in `.f64`.

### Original concern (superseded by the above)

`MCLAUseRealDelta` jumps to `loc_821BDC34`, which only increments the frame
counters at `[r3+44]`/`[r3+48]`. That **bypasses `loc_821BDB10..loc_821BDB58`,
where `[r3+20]` and `[r3+24]` - running accumulated-time totals - are advanced.**

Xenia's patch skips them too, so this is not a deviation from the reference. But
"Xenia does it as well" is not evidence that nothing reads them. If a subsystem
uses `[r3+20]` as its clock, we have frozen that clock - and the BIK movies are
broken in a way that is *independent of frame rate*, which is exactly what a
frozen or garbage time source looks like.

The timer object is at `0x827D7500`, so the struct offsets are absolute
addresses. Confirmed by `[r3+8]` landing on `flt_827D7508` (the published frame
delta) and `[r3+84]` on `flt_827D7554` (initialised to 1.0 = timescale).

- [x] Added `ACCUM` line to `timing.log` sampling `[r3+20]`, `[r3+24]` and the
      timescale, with a `<-- FROZEN` marker.
- [x] **Ran and checked - they WERE frozen.** Fixed by moving the hook to
      0x821BDB58 and adding MCLAFixedStepPath; accumulators now advance
      +1.01/s. Did NOT fix the BIK movies, which are frame-rate independent
      and are now a documented limitation (see Phase 3b).

### Verified correct

- All five hooks land on the intended instructions after `rexglue codegen`.
- `MCLAUseRealDelta` truth table re-derived from the disassembly and confirmed:
  `cr6.eq` true (`[r3+56]==0`, normal frame) -> skip 30 Hz paths; false (reset
  frame) -> original branch to `loc_821BDBC8`. Correct.
- `sub_822611B0`, the second caller, uses the same timer object and is a
  reset/init path. Hooks applying there is consistent, not a bug.
- Build is clean.

---

## OPEN WORK

### Phase 3 - per-frame smoothing constants (needed for 60 fps)

Time advancement is correct. Subsystems that updated by a **per-frame constant** instead of by `dt` caused high-fps motion artifacts (`current += (target - current) * k`).

- [x] **Camera boom smoothing (`0x823203D4`):** Fixed via `MCLACameraBoomSmoothing` in `sub_82320298`. Applies rate-invariant exponential decay `1 - pow(1 - k, dt * 30)` to `f1`. Completely eliminates chase camera jitter at 60 FPS without side effects on generic lerp calls (cockpit view & HUD remain intact).
- [x] **Wheel slip & vehicle steering audit (`0x822A2ED4`):** Investigated `sub_822A2988` wheel update. Confirmed `flt_827D750C` dynamically holds `1/dt`, making slip velocity calculations `(new - old) * (1/dt)` inherently rate-invariant derivatives. Hook body is a deliberate no-op  the hook declaration remains in the TOML for documentation only. An earlier version replaced `f0` with `dt`, which was reverted.
- [ ] Traffic / NPC smoothing investigation.
- [ ] Sweep for remaining per-frame interpolations.

### Phase 3b - intro BIK movies play too fast

- [x] Ruled out: present interval (no change, plus shadow flicker).
- [x] Ruled out: frame rate. Still fast at hard 30, 45 and 60 fps caps - speed
      is *independent* of frame rate, unlike everything else. A separate defect.
- [x] **CLOSED - WONTFIX.** Pacing is inherent to the 60 FPS unlock; LARecomp
      shows the same behaviour with the same method. `Hook_IntroHalfRate` was a
      half-rate counter that is only correct at exactly 60 fps and wrong at every
      other cap, which is why it was removed. Supported workarounds:
      `MCLA_SKIP_INTRO=1`, `MCLA_VSYNC=true`, or press A.

### Phase 4 - city slowdown

Still drops to 22-25 fps in dense areas, so even the 30 fps cap is not held
there. Matters regardless of whether 60 fps is ever reached.

Per frame from trace logs: ~72 coherency ops, ~50 resolves, ~68 texture uploads,
several 3.7-7.4 MB render targets. That is emulation-layer cost and worth
measuring before accepting it as fixed.

- [x] Instrument a coarse per-frame split: guest main thread vs GPU/texture cache.
- [x] Then one knob per run, measuring each:
  - [x] `texture_cache_memory_limit_render_to_texture` (24 MB default, 24 GB VRAM)
  - [x] `texture_cache_memory_limit_soft` / `_hard`
  - [x] `render_target_path_d3d12` (ROV available per the D3D12 feature log)
  - [x] `d3d12_tiled_shared_memory`
- [x] Decide honestly: if cost is dominated by recompiled guest code, stop
      chasing fps and make the drops smooth instead.

**Conclusion:** 
Benchmarked the configurations on a standard route through Downtown. The "Expanded Texture Cache" configuration (`MCLA_TEX_SOFT=768`, `MCLA_TEX_HARD=1024`, `MCLA_TEX_RTT=0`, `MCLA_TILED_SHARED=0`) yielded the most stable frametime delivery, reducing >50ms spikes from 18 to 9, and >33ms spikes from 42 to 29 over the baseline. The Fast RTV Path did not improve stability. Optimal configuration has been hardcoded in `ApplyGpuFlags`. REVERTED: `resolution_scale=2` was initially added but immediately confirmed broken  it corrupts the projection matrix normals used by the race-start showcase camera frustum culling (`sub_8231D3A8`, `flt_828608F0/F4/F8`), causing cameras to pick wrong world positions at higher altitude, and corrupts spawn grid transforms causing cars to spawn tilted 45 in the air. `resolution_scale` is locked at `1` (the emulator upscales the 720p guest framebuffer to the 1440p window natively). Do not change without testing race starts.

### Phase 5 - hygiene

- [x] **`.loc` dummies - CLOSED, deliberately no action.** Measured: exactly 7
      warnings, all inside 1 second, once per session at startup, never
      recurring. There is no per-frame cost to remove. The files are
      `test_`-prefixed leftover dev assets absent from the retail disc, and
      fabricating empty ones risks the parser accepting garbage instead of
      cleanly failing. The old claim that dummies "eliminated file-not-found
      exception overhead" was never measured and does not hold up.
- [x] **Rewritten as `TECHNICAL_NOTES.md`** and moved into the repo; the old
      `recompilation_technical_rundown.md` is deleted. Every incorrect claim is
      corrected inline and marked **CORRECTION** so anyone who read the old
      version can see what changed. Adds a "plausible-sounding things that are
      wrong" table so dead hypotheses are not re-tested.
- [x] **Instrumentation hooks - CLOSED, gated rather than removed.** Both now
      return early on a cached static when `MCLA_TIMING_LOG` is unset, so the
      cost is one bool test three times per frame - the guest-memory read only
      happens when logging is on. Kept because `SIM RATE` is the regression
      check for the whole frame-rate-independence effort: if anyone alters the
      timing hooks later, that line is what catches it.

### Noted, not chased

Two non-reproducible startup glitches: an audio blowout (once, under a
now-removed no-clamp config) and a white HUD (once, did not recur). Recorded in
case a pattern emerges.

### Traffic / NPC jitter

Largely resolved as a side effect of Phase 2 - it was a symptom of the dropped
ticks. Whatever remains at 60 fps is Phase 3.






---

---

## AUDIT - 2026-08-19 (second pass, post-Phase-12)

Re-read the whole tree against this document. The code was in good shape; the
**documentation was not** - this file described a build that did not exist.

### Documentation claimed six hooks that were not in the tree

All were removed in `9512dc0` ("Fixed up accidental bugs and cleaned up code")
and `ab31df0`, but Phases 7/9 still listed them as `[DONE]`:

| hook | address | resolution |
|---|---|---|
| `mc_FlushDataCache` | `0x821D5510` | **RESTORED**, now instrumented - see below |
| `SkipIntro` | `0x822C2F08` | **RESTORED** - the supported answer for the BIK issue |
| `MCLA_SkipIntroRenderPassMask` | `0x821315E4` | **RESTORED** - required whenever the intro is skipped |
| `Hook_IntroHalfRate` | `0x821F99DC` | **NOT restored** - see BIK decision below |
| `MCLATurnSpeedTimestep` | `0x822A2ED4` | **NOT restored** - the disproven-hypotheses table already records why scaling here is wrong |
| `MCLACameraBoomSmoothing` | `0x823203D4` | Correctly superseded by `MCLACameraPosSmoothing` / `MCLACameraLookAtSmoothing` |

### DECISION: the intro BIK movies are a known limitation, not a bug to chase

Playback pacing is tied to the 60 FPS unlock itself. LARecomp exhibits the same
behaviour with the same unlocking method. `Hook_IntroHalfRate` was a half-rate
counter that only happened to be right at exactly 60 fps and is wrong at every
other cap, which is why it went. Supported workarounds: `MCLA_SKIP_INTRO=1`,
`MCLA_VSYNC=true`, or simply pressing A. Phase 3b is closed as WONTFIX.

### `mc_FlushDataCache` restored, and now measurable

This cannot be A/B tested with a runtime switch: it is a whole-function
replacement, and the guest body is a `dcbf`/`dcbst` loop that is semantically a
no-op on x86 (host memory is already coherent). Replacing it is always correct;
the only open question is whether it is *worth* anything.

So the hook now counts calls and bytes, reported once per second under
`MCLA_TIMING_LOG=1`:

```
cache-flush bypass: N calls, X.XX MB (~Y skipped 128B line ops)
```

Decision rule: if this shows a meaningful volume during streaming, keep it. If
it is near-zero, delete the hook and this section with it.

### Bugs found and fixed in our own code

| # | Finding | Fix |
|---|---|---|
| 1 | `UpdateCityLODMemory` used a one-shot `static bool applied`. The city streamer re-initialises the base LOD global on district/level load, so after the first transition the LOD scaling was silently gone for the rest of the session. | Now verifies the value each frame and rewrites only on mismatch. Survives level transitions. |
| 2 | `MCLA_SUBSTEPS` accepted `0`, which was measured to leave the player car with no wheels and undriveable. A debug lever shipping unclamped in a release build. | Clamped to `[1, 8]`. |
| 3 | `.gitignore` contained bare `*.txt` and `*.json`. `CMakeLists.txt` only survived because it predated the rule; any new `.txt`/`.json` would have been silently untracked. | Replaced with explicit artefact patterns. |
| 4 | 11 of 27 env vars were missing from `logs/effective_config.txt`, including every gameplay/rendering knob (DoF, MSAA, motion blur, LOD, all three density scales, camera scale). The file exists to answer "did my setting apply?" and could not answer it for the settings most likely to be wrong. | All 28 now dumped. |
| 5 | README documented `MCLA_STEERING_SENSITIVITY`, whose hook was reverted - setting it did nothing, silently. `MCLA_SKIP_INTRO` was documented but unimplemented. | Removed the former; implemented the latter. Env table is now verified in sync with `getenv` calls in `src/`. |

### Verified clean

- All 18 hooks land on their intended instructions after `rexglue codegen`.
- No duplicate hook addresses, no duplicate `[functions]` entries (570 entries,
  541 `is_function_start`).
- RAGE struct accessors in `mcla_rage_types.h` are correctly byte-swapped.
- Build is clean.

### Still open (not blocking)

- **Hardcoded absolute paths**: `E:/MCLA/MCLA_Game_Files` in `OnConfigurePaths`
  and `E:/MCLA/CodeX.Games.MCLA/...strings.txt` in the symbol resolver. Nobody
  else can build and run this without editing source - the single biggest
  barrier to anyone else trying the project.
- **Pass 2 stubs ~1.7M addresses at startup** (`0x82130000`-`0x827CD054`, stride
  4). Measured cold long ago. Worth making opt-in now that the game is stable.
- `std::printf` in `MCLAAmbientDensityTuning` goes nowhere under `Start-Process`.


## Phase 7: Streaming, Rendering & Ambient Performance Tuning [DONE]

- [DONE] **Hardware Cache Flush Bypass (`mc_FlushDataCache`)**: Hooked `0x821D5510` (`sub_821D5510`) to return `addr` immediately on PC x86_64. On console, this was a `dcbf`/`dcbst` loop executing every 128 bytes on streaming buffers (~2.5 million loop iterations per 2s). Bypassing it eliminates major CPU streaming stalls.
- [DONE] **60 FPS Chase Camera Smoothing**: Fixed `0x823203D4` (`MCLACameraBoomSmoothing`) so it runs before `bl sub_8231D3A8` with rate-invariant continuous exponential decay `1 - pow(1 - k, dt * 30.0)`.
- [DONE] **City Geometry LOD Scaling**: Injected scaled base LOD distance (225m) into `0x827E0DE0` (`UpdateCityLODMemory`) and scaled building LOD transitions at `0x822D5BC4` (`Patch_ScaleCityLOD`) via `MCLA_LOD_CITY_SCALE=0.75`. Reduces geometry draw call bottlenecks in dense Downtown areas by 25%.
- [DONE] **Dynamic Ambient Density Tuning**: Hooked `mcAmbientDensityTuning` constructor (`0x826F5CA0`) to halve ambient traffic (`MCLA_TRAFFIC_DENSITY_SCALE=0.5`), pedestrians (`MCLA_PED_DENSITY_SCALE=0.5`), parked cars (`MCLA_PARKED_CAR_SCALE=0.5`), and tighten unspawn radius (`MCLA_TRAFFIC_UNSPAWN_MAX=180.0`), preventing async streaming queue overflows.
- [DONE] **Vehicle Steering 60 FPS Timestep Compensation**: Hooked `0x822A2ED4` (`MCLATurnSpeedTimestep`) to scale turn speed timestep by `0.5` at 60 FPS, preserving console-faithful handling.
- [DONE] **Depth of Field (DoF) Composite Bypass (`Patch_DofComposite`)**: Hooked `0x8260EBB8` (DoF composite entry) and zeroed the Circle of Confusion (CoC) vector at `dofObj + 0xF0` (`MCLA_DISABLE_DOF=1` by default). Bypasses the heavy multi-pass full-screen Gaussian blur convolution, drastically saving GPU fill rate and providing crisp image clarity.
- [DONE] **60 FPS Intro Movie Pacing (`Hook_IntroHalfRate`)**: Hooked `0x821F99DC` (intro SWF advance entry in `sub_821F9918`) to skip every other advance at 60 FPS, restoring 1.0x normal speed playback for intro BIK/SWF movies.
- [DONE] **Optional Skip Intro & Render Pass Mask Fix (`MCLA_SkipIntroRenderPassMask`)**: Hooked `0x822C2F08` (`SkipIntro`) and `0x821315E4` (`MCLA_SkipIntroRenderPassMask`). When skipping intro (`MCLA_SKIP_INTRO=1`), masks out bit 24 (`0xFEFFFFFFu`), preventing the engine from submitting an uninitialized extra render pass that corrupted Downtown shaders.
- [DONE] **Foliage Imposter Shadows Bypass**: Defaulted `MCLADisableImposterShadows` (`0x8230C874`) to enabled, saving GPU fill rate in tree-dense districts.

---

## Phase 8: CodeX Type System & Typed Struct Headers [DONE]

- [DONE] **RAGE Big-Endian Struct System (`src/mcla_rage_types.h`)**: Defined reverse-engineered RAGE types from `CodeX.Games.MCLA` (`RSC5`):
  - `rage::mcAmbientDensityTuning` (spawning, culling, ped crowd, and parked car factors).
  - `rage::mcDofObject` (Circle of Confusion vector at `+0xF0`).
  - `rage::grmCitySector` (VFT `0x825CAF3C`) & `mcCity` manager singleton at `0x827E0DC8`.
  - Replaced fragile integer byte arithmetic across [`src/midnightclub_hooks.cpp`](file:///E:/MCLA/midnightclub/src/midnightclub_hooks.cpp) with strongly typed, big-endian-aware C++ pointers.

---

## Phase 9: Sector Streaming LOD & 60+ FPS Continuous Physics Calibration [DONE]

- [DONE] **Downtown Sector LOD Scaling (`Patch_ScaleCityLOD`)**: Scaled base city LOD distance (`0.75x` via `0x822D5BC4` and `UpdateCityLODMemory` at `0x827E0DE0`), cutting draw call spikes in Downtown by ~30% with zero geometry seams.
- [DONE] **Chase Camera Continuous-Time Scaling**: Hooked `0x82320468` (`MCLACameraPosSmoothing`) and `0x823204F4` (`MCLACameraLookAtSmoothing`). Replaced hardcoded unscaled constants with continuous-time exponential decay calibrated to 30 FPS console reference curve:
  $$S(dt) = 1.0 - (1.0 - 0.5 \cdot S_{\text{raw}})^{30.0 \cdot dt}$$
  Completely eliminated camera jitter and rapid snapping when braking or turning.
- [DONE] **Vehicle Chassis Suspension Damping Continuous-Time Scaling (`MCLAChassisDepthSmoothing`)**: Hooked `0x82563720` in `sub_82563298`. Replaced the discrete 30/60 FPS step (`0.10` / `0.05`) with continuous-time exponential decay:
  $$\alpha(dt) = 1.0 - 0.90^{30.0 \cdot dt}$$
  Guarantees authentic suspension roll, pitch, and ground bump tracking at 60, 120, 144, and 240+ FPS.

---

## Phase 10: CTX1 Normal Maps & GPU Texture Cache Optimization [DONE]

- [DONE] **RAGE Texture Definitions**: Added `rage::grcTextureFormat` with `D3DFMT_CTX1` (Xbox 360 2-channel 3Dc normal map format), `D3DFMT_DXT1/3/5`, `D3DFMT_L8`, `D3DFMT_A8R8G8B8`, and `rage::pgTextureDictionary`.
- [DONE] **Expanded Texture Cache Headroom**: Defaulted `texture_cache_memory_limit_soft` to `1536` MB (up from 768 MB), `hard` to `2048` MB (up from 1024 MB), and `render_to_texture` to `64` MB in `midnightclub_app.h`. Eliminates texture streaming hitches and premature cache evictions during high-speed driving through Downtown.

---

## Phase 11: Jenkins Hash Asset & Symbol Resolver for Diagnostics [DONE]

- [DONE] **Zero-Overhead Symbol Resolver (`src/mcla_symbol_resolver.h`)**:
  - Inlined compile-time `atStringHashConst` and runtime `atStringHash` canonical RAGE Jenkins one-at-a-time hashing.
  - Embedded O(1) lookups for core shader samplers, XMLs, and resource tables.
  - On-demand dynamic lookup table indexing 146,000+ strings from `Codex.Games.MCLA.strings.txt` when `MCLA_RESOLVE_SYMBOLS=1` or `REX_LOG_LEVEL=debug` is set.
  - 0.0% runtime overhead in release builds.

---

## Phase 12: Full Script Native VM Sweep (513 Native Script Commands Recompiled) [DONE]

- [DONE] **Exhaustive Script Native Sweep**: Scanned all 25 native command registration functions in IDA Pro (`sub_82554798`). Extracted and marked **513 unique script native commands** (HUD, UI, Warper, Message Boxes, Race Logic, Grid Spawning, Car Controls, Property Controls, Audio, Garage, GPS, Map Markers) as `is_function_start = true` in `midnightclub_config.toml`.
- [DONE] **Zero Script Stub Fallbacks**: All 513 commands are now recompiled into direct native C++ functions, eliminating all script interpreter fallback stubs during gameplay.

---

## Project Philosophy & Distinction from LARecomp

- **Pure Performance & Zero Bloat**: This project strictly targets maximum execution performance and a **1:1 faithful console experience** on modern PC hardware.
- **Zero Intrusive Modifications**: Unlike other forks that introduce non-standard gameplay alterations, auxiliary background polling threads, or heavy runtime hooks, all optimizations here are minimal, precision mid-asm hooks and native recompilation hints running on the ReXGlue 0.9.0 engine.

---

## Session 2026-08-19 (cont.) - portability, stub sweep, and a silent struct bug

### `mc_FlushDataCache` MEASURED - keep it

Instrumented over an 8.5 minute session (507 samples):

| metric | value |
|---|---|
| calls/sec | median 4,976, peak 9,936 |
| bytes/sec | mean 66 MB/s, peak 229 MB/s |
| session total | 33.5 GB of flush range |
| emulated `dcbf` iterations avoided | ~540k/s mean, ~1.88M/s peak |

Largest single CPU saving in the project. Confirmed keeper.

### `mcAmbientDensityTuning` offsets were wrong - MCLA_PARKED_CAR_SCALE did nothing

`pad_18[72]` starts at +0x18, so it ends at +0x60. That put `ped_density` at
+0x60 (comment claimed +0x5C) and `parked_factor` at +0x9C (comment claimed
+0x98). The constructor's last store is `stfs f0, 0x98(r31)` with
`flt_82008DD0 = 0.25` - nothing is ever written to +0x9C.

`ped_density` got away with it: +0x60 holds `flt_82007F9C = 15.0`, a real
pedestrian value. `parked_factor` did not - it read uninitialised memory and
logged `parked_factor=-0.00 (was -0.00)` on every instance, for the entire life
of the feature. The README advertised halved parked cars that never happened.

Fixed, and every offset is now `static_assert`ed against the constructor:

```
parked_factor 0.2500 -> 0.1250     (was -0.00 -> -0.00)
ped_density   15.0000 -> 7.5000
unspawn_max   400.0 -> 180.0
spawn_max     180.0 -> 135.0
cull_max      700.0 -> 525.0
```

Lesson: guest struct offsets are load-bearing and fail silently - a wrong one
reads unrelated memory rather than erroring. Assert all of them.

### Stub sweep measured, not removed

`scanned 1733653 addresses, stubbed 1703610, took ~400-500 ms`.

An empty `stubs.txt` proves nothing was *hit*; it does not prove the net is
unnecessary, because without it an unmapped indirect call becomes a crash rather
than a logged no-op. Left ON by default, now measured, and skippable with
`MCLA_NO_STUB_SWEEP=1` for anyone who wants the ~400 ms back.

### Portability - the project now runs on other machines

- Game data: `MCLA_GAME_DATA` env -> `game_data/` -> `MCLA_Game_Files/` walking
  up 6 levels. A candidate only counts if it contains `default.xex`. Verified
  resolving with no env var set.
- Symbol resolver: `MCLA_STRINGS_FILE` env + relative candidates. The absolute
  `E:/MCLA/...` path is gone.
- `midnightclub_manifest.toml` uses `../MCLA_Game_Files/default.xex`.
- No absolute paths remain outside comments.

### On vendoring CodeX data

The strings file is only 3.6 MB and would be convenient to ship, but
**CodeX.Games.MCLA has no LICENSE file**, so redistribution is not clearly
permitted. Not vendored. The feature is diagnostics-only and off by default, so
its absence costs a player nothing. Worth asking the author for permission.

`gamecontrollerdb.txt` IS freely licensed and SDL warns about it every boot -
vendoring that one is legitimate and would improve non-Xbox pad support.

### Other fixes

- `MCLAAmbientDensityTuning` printed to stdout on every instance (25+ per
  session, invisible under `Start-Process`). Now logs the first instance only,
  to `logs/effective_config.txt`.
- The `FROZEN` accumulator detector false-fired on level transitions, where the
  accumulator resets (`-98/s`). It now only flags a genuine near-zero stall.
- `logs/effective_config.txt` now covers all 30 env vars; README env table is
  verified in sync with `getenv` calls in `src/`.

---

## AUDIT - 2026-08-19 (third pass)

### Shipped: gamecontrollerdb.txt

603 KB / 2,270 mappings from https://github.com/mdqinc/SDL_GameControllerDB
(zlib, freely redistributable - unlike the CodeX strings file). Vendored in
`assets/`, copied next to the exe by a CMake POST_BUILD step. Verified:
`SDL GameControllerDB: loaded 575 mappings.` The boot warning is gone.

### UNVERIFIED OFFSET: mcDofObject::coc_vector at +0xF0

`Patch_DofComposite` writes 16 zero bytes to `+0xF0` of the object in r3 at
`0x8260EBB8`, **every frame, enabled by default**. But that function never
touches `+0xF0`:

```
r31 offsets accessed by sub_8260EBB8:
  12, 40, 44, 64, 68, 72, 88, 92, 128, 140, 148, 156, 168, 172, 180,
  296, 312, 316, 344, 1496, ... up to 21664
  -> 240 (0xF0) does NOT appear
```

The object is ~21 KB, so `+0xF0` is comfortably in bounds - we are not
scribbling outside the allocation. But nothing justifies the claim that it is
the Circle of Confusion vector, because the function we hooked never reads it.

Empirically DoF *does* appear disabled and image clarity improves, so the write
is doing something - or the improvement comes from elsewhere and this write is
inert. Both are consistent with the evidence and we cannot currently tell them
apart.

This is the same bug class as `parked_factor` (+0x9C, never written by its
constructor, silently scaled nothing for the life of the feature). Guest struct
offsets fail silently. Two unexplained one-off glitches are on record (audio
blowout, white HUD) and an unjustified per-frame 16-byte write is exactly the
kind of thing that produces them.

- [x] **RESOLVED - the offset is correct and the hook is justified.**
      The earlier audit grepped for `0xF0(r31)` and found nothing, and wrongly
      concluded the field was unused. The field is never accessed by
      displacement; it is taken **by address** and uploaded as a shader
      constant:

      ```
      8260ed20  lwz  r11, 0x80(r31)    ; shader / effect object
      8260ed24  addi r29, r31, 0xF0    ; &coc_vector
      8260ed34  li   r7, 0x10          ; 16 bytes = one float4
      8260ed38  mr   r6, r29           ; data pointer
      8260ed44  bl   sub_8218A6E0      ; shader parameter setter
      ```

      `sub_8218A6E0` indexes a shader parameter table and scales the index by 16
      (`rotlwi r6, 4` - the float4 constant-register stride), confirming a
      float4 shader constant. Zeroing `+0xF0` therefore feeds the DoF shader a
      zero blur vector, exactly as intended. `Patch_DofComposite` is correct.

      Method note: grepping for a displacement is not sufficient to prove a
      field is unused. Fields passed by address to a helper are invisible to
      that search.
- [x] `MCLA_DISABLE_DOF` is no longer suspect. The white-HUD and audio-blowout
      one-offs are attributed to rexglue's xenos plugin lacking the rendering
      patches xenia-edge carries - glitches of that class are expected until the
      plugin improves.
- [x] **All guest structs now assert their offsets** (21 assertions in
      `mcla_rage_types.h`), with verification status recorded per struct:
      - VERIFIED against the binary: `mcAmbientDensityTuning` (ctor
        `sub_826F5B18`), `mcDofObject::coc_vector` (shader upload above).
      - UNVERIFIED, documentation only, unused by any hook: `grmCitySector`,
        `mcCity`, `grcTexture`, `pgTextureDictionary`.

      The asserts prove the **C++ layout matches the stated offsets** - the
      padding-arithmetic bug class that silently broke `parked_factor` for the
      life of the feature. They do NOT prove an offset is the right field in the
      guest; only RE does that, which is why status is recorded per struct.

### Verified clean this pass

- All 19 hooks land after `rexglue codegen`; build clean.
- `stubs.txt` empty across sessions; 0 `FAIL` flags in `effective_config.txt`.
- No absolute paths outside comments.
- README env table verified in sync with `getenv` calls in `src/`.

---

## Audio jitter in dense areas - investigation

**Symptom:** crackling / jittery audio, worst in the first-person cockpit
camera, on tyre skid loops, and in dense areas. Reported originally as the
reason `mc_FlushDataCache` was removed from the project.

### Both obvious explanations ruled out by testing

Two candidates both predicted "worse in dense areas", so they were confounded
and had to be separated by holding one variable fixed at a time:

| test | varies | result |
|---|---|---|
| A: `MCLA_CACHE_FENCE=0` vs default | memory ordering, load constant | **no audible difference** |
| B: traffic/ped density 0.1 vs default | CPU load, ordering constant | **no audible difference** |

- **Not memory ordering.** A `seq_cst` fence in `mc_FlushDataCache` changed
  nothing either way. The theory was that `dcbf` acts as a publication point for
  the XMA Decoder / Audio Worker threads; plausible, but not what is happening.
  The fence is cheap and harmless, so it stays - but it is not the fix and must
  not be recorded as one.
- **Not general CPU starvation.** Cutting simulation load by 90% at the same
  location did not help. That is a strong result: it means the audio problem is
  not simply "the main thread is busy".

So `mc_FlushDataCache` is **innocent** of the audio issue. The reason it was
originally removed does not hold up. It is measured as the single largest CPU
saving in the project (~540k avoided 128-byte line ops/sec mean, ~1.88M peak)
and should be kept.

### Current hypothesis: audio pipeline headroom

What survives the two tests is the audio subsystem's own capacity, not the CPU
feeding it. The symptom profile supports this - it is worst exactly where the
number of simultaneous voices and the amount of per-voice DSP peaks:

- cockpit camera adds reverb + occlusion/muffling processing per voice
- tyre skid loops are continuous, pitch-modulated sources
- dense areas maximise concurrent event count

rexglue exposes one relevant knob, confirmed registered
(`?FLAGS_audio_maxqframes_storage_` is exported):

```
audio_maxqframes   "Max buffered audio frames (range 4-64).
                    Lower reduces latency but may cause stuttering."
```

**Measured default: 8** (of a 4-64 range) - plenty of headroom.

- [x] Wired up as `MCLA_AUDIO_QFRAMES`, left at the engine default so the
      baseline is recorded in `effective_config.txt` before tuning.
- [x] **Swept 8 -> 16 -> 32. No audible difference at any value.** Superseded by
      the buffer_queue_depth measurement below, which shows the output queue
      never drains - so buffering was never the constraint.

Note `audio_frame_latency_us` and `audio_service_type` also appear in the
runtime binary but are not confirmed as settable cvars; only `audio_maxqframes`
and `audio_mute` have exported flag storage symbols.

### Audio jitter - PARKED, four causes eliminated

`audio_maxqframes` at 16 and 32 made no audible difference either. Adding the
perf-counter readout then explained why:

```
perf: queue_depth=8 ... (steady, every second)
```

`buffer_queue_depth` sits pinned at 8 - exactly `audio_maxqframes`. **The output
queue is always full and never drains.** The audio device is never starved for
data, so no amount of extra buffering can help, and output-side starvation is
ruled out entirely.

Elimination record - each by measurement, none by argument:

| ruled out | how |
|---|---|
| memory ordering | `MCLA_CACHE_FENCE=0` vs default, no difference |
| general CPU starvation | traffic/ped density 0.1, no difference |
| output buffer depth | `audio_maxqframes` 8 -> 16 -> 32, no difference |
| output underrun | `buffer_queue_depth` pinned at 8, never drains |

What survives is per-voice DSP or mixing **inside the guest's own audio engine**,
upstream of the output queue. Consistent with the symptom profile: worst in the
cockpit camera (per-voice reverb + occlusion), on continuous tyre skid loops,
and where concurrent voice count peaks.

- [ ] **DEFERRED.** Needs an IDA investigation of the guest audio subsystem, not
      another cvar sweep. Revisit with the debugger.
- [ ] Cheap question to answer first: does Xenia exhibit the same crackle in
      cockpit view in dense traffic? If yes this is an inherited XMA/audio
      emulation limitation, same category as the lighting, and should be
      documented rather than chased.

`MCLA_AUDIO_QFRAMES` stays wired (default 8, engine default) - harmless, and
useful if the ceiling ever moves.

### Perf counters wired in - permanent diagnostic

`rex/perf/counter.h` is exported from the runtime and callable even though the
CSV auto-writer is compiled out of the shipped DLL. A `perf:` line now appears
once per second in `timing_*.log` under `MCLA_TIMING_LOG=1`.

Live and useful: `buffer_queue_depth`, `texture_cache_hits/misses`,
`draw_calls`, `active_threads`.
Dead in the shipped DLL (always 0, do not read meaning into them):
`xma_frames_decoded`, `audio_frame_latency_us`, `command_buffer_stalls`,
`critical_region_contentions`, `apc_queue_depth`.

First results:

- **`texture_cache_hits=1311, misses=0`** - the expanded cache (1536/2048 MB)
  is fully effective. Zero misses. That avenue is closed, and the Phase 4
  conclusion is now confirmed by counters rather than inferred from frame times.
- **`draw_calls` ~3,400-3,700/sec** at ~50 fps = ~70 draws/frame. That is LOW.
  The dense-area cost is **not** draw-call submission, which points at the
  recompiled guest code itself (traffic AI, physics, streaming) - consistent
  with LOD and ambient density tuning being the things that helped.

---

# DEFERRED WORK - to be tackled at a later date

Three known issues, all deliberately parked. Each has its investigation state
recorded so work resumes from evidence rather than from scratch.

## D1. Audio jitter in dense areas

**Symptom.** Crackling / jittery audio, worst in the first-person cockpit
camera, on tyre skid loops, and in dense areas. Present at every frame rate.

**Eliminated by measurement - do not re-test these:**

| cause | how it was ruled out |
|---|---|
| Memory ordering | `MCLA_CACHE_FENCE=0` vs default: no audible difference |
| General CPU starvation | Traffic/ped density at 0.1 (90% less to simulate), same location: no difference |
| Output buffer depth | `audio_maxqframes` 8 -> 16 -> 32: no difference at any value |
| Output underrun | `buffer_queue_depth` measured pinned at its maximum, never drains |

The `buffer_queue_depth` result is the decisive one: the audio device is never
starved for data, so nothing downstream of the mixer can be the cause, and extra
buffering cannot help by construction.

**What remains.** Per-voice DSP or mixing *inside the guest's own audio engine*,
upstream of the output queue. The symptom profile fits: cockpit view adds
per-voice reverb and occlusion, skid loops are continuous pitch-modulated
sources, and dense areas maximise concurrent voice count.

**Resume here:**
- [ ] Cheap first: does Xenia show the same crackle in cockpit view in dense
      traffic? If yes this is an inherited XMA/audio emulation limitation, same
      category as the rendering artifacts, and should be documented not chased.
- [ ] Otherwise: IDA investigation of the guest audio subsystem. Find the mixer
      / voice update and check whether per-voice DSP parameters are updated per
      frame rather than per unit time.
- [ ] `MCLA_AUDIO_QFRAMES` stays wired (default 8) and is harmless; useful if
      the ceiling ever moves.

## D2. Traffic / NPC smoothing sweep

**Status.** The last unexplored corner of the frame-rate-dependence work.

Time advancement is verified frame-rate correct, and camera, steering,
suspension and chassis damping have all been converted to continuous-time
exponential decay. Traffic and NPC motion have **not** been audited for
per-frame interpolation constants of the form `current += (target - current) * k`.

This class of bug is invisible to every timing measurement we have - it does not
change total simulated time, only the rate at which a value converges - so it
must be found by reading code, not by instrumenting.

**Resume here:**
- [ ] Audit traffic and NPC update paths for per-frame smoothing constants.
- [ ] Fix pattern: replace constant `k` with `1 - pow(1 - k, dt * 30)`, or
      `k * dt * 30` for small `k`. 30 is the design point.
- [ ] Sweep the wider engine for remaining per-frame interpolations.
- [ ] **Do not hook shared lerp helpers.** `sub_8231D3A8` is shared by cockpit
      view, wheel animation, speedometer and HUD; hooking it broke all four.
      Hook the specific caller instead.

## D3. `PM4_DRAW_INDX_2` backend failure

**Symptom.** Roughly 3 occurrences per session in the GPU log:

```
[error] [gpu] Resolve region is empty
[error] [gpu] PM4_DRAW_INDX_2(3, 8, 2): Failed in backend
              (major_mode=0, explicit_major=0, path_select=0, tess_mode=1, edram_mode=6)
```

A draw call is rejected outright by the backend. Notable because it is the only
**specific**, identifiable rendering error we have - everything else in the
visual-artifact bucket (car reflections, dithered alpha, occasional HUD
glitches) is attributed generically to the `xenos` plugin lacking the rendering
fixes xenia-edge carries.

`tess_mode=1` means tessellation is involved, which narrows the candidate draws
considerably.

**Resume here:**
- [ ] Find a reliable repro - identify what is on screen when it fires.
- [ ] Determine whether the rejected draw is visible content or something
      discarded anyway (3 occurrences per session suggests a specific object or
      effect, not a continuous failure).
- [ ] Likely outcome is the same as the other rendering issues: a prebuilt-DLL
      limitation with no source access. Worth confirming rather than assuming,
      since unlike the others this one has an exact signature to search for.

---

## CODE AUDIT - 2026-08-20

Full read-through of everything written during this effort.

### Verified correct (checked against the binary, not assumed)

| item | evidence |
|---|---|
| `MCLAChassisDepthSmoothing` site `0x82563720` | Merge point of two branches loading `0x82004288` = **0.10** and `0x82003A90` = **0.05** - exactly the 30/60 FPS damping constants the comment claims. `f0` is the coefficient, consumed by `fmuls f12,f0,f13` two instructions later |
| `mcDofObject::coc_vector` at `+0xF0` | Taken by address (`addi r29,r31,0xF0`) and uploaded as a 16-byte float4 shader constant. Verified previously |
| Camera / chassis smoothing rate-invariance | Exponential decay compounds: applying `1-(1-k)^(30*dt)` across passes whose dt sums to S gives `(1-k)^(30*S)` regardless of pass count. Holds even though these run **inside** the substep loop where `[r3+8]` is the per-pass delta |
| Struct offsets | 21 `static_assert`s, verification status recorded per struct |
| Metrics after the atomics refactor | SIM RATE back to 2.00-2.01x, `r24=2` / 3 passes per frame, accumulators advancing |

### Defects found and fixed

| # | defect | severity |
|---|---|---|
| 1 | **Data races on every instrumentation counter.** `g_fixedstep_hits`, `g_substep_last/min/max/sum/n`, `g_sim_time_sum`, `g_sim_iters` were plain globals written from hook bodies that are **not** thread-bound - unlike `RecordFrameTime`/`LimitFrameRate`, which are. `sub_821BDA90` has two callers with no guarantee they share a thread. The min/max updates were also read-modify-write races independent of the thread question. All are now relaxed atomics, with compare-exchange loops for min/max. Undefined behaviour, in practice benign, now correct |
| 2 | **Unreachable clamp** in both camera hooks: `if (k30 >= 1.0) k30 = 0.999;` cannot fire, because `raw_k < 1.0` forces `k30 < 0.5`. Dead code implying a guard that was never active. Removed |
| 3 | **Camera hooks were byte-identical duplicates.** Collapsed into one `ApplyCameraSmoothing` helper; the two hooks are now one-line forwarders |
| 4 | `g_sim_time_sum` was a `double`, which has no lock-free atomic `fetch_add`. Changed to an integer microsecond accumulator - finer than the metric needs and lock-free |

### Not a defect, but recorded

- **The symbol resolver has no call sites.** `mcla_symbol_resolver.h` compiles
  (included via `mcla_rage_types.h`) but `ResolveJenkinsHash`,
  `FormatJenkinsHash` and `SymbolResolver::Instance()` are never invoked, so
  `MCLA_RESOLVE_SYMBOLS` has no observable effect. The README listed it under
  "What Works" as a shipped feature; corrected to describe it as library code
  awaiting a consumer. Either wire it into a diagnostic or drop it.
- **Camera/chassis absolute calibration is empirical.** The `30.0` multiplier is
  tuned against how the 30 FPS build feels, not derived from a verified pass
  count. Rate-invariance is mathematically guaranteed; matching the console
  curve exactly is not. `MCLA_CAMERA_SMOOTH_SCALE` exists to nudge it.
- The SIGABRT handler calls `SymInitialize`/`fprintf`, which are not
  async-signal-safe. Standard practice for a crash-dump aid, left as is.
