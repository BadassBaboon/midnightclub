# MCLA Recomp — Work Plan

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

**Recommended for playing: `MCLA_FPS_CAP=30`.** Correct physics, correct camera,
correct traffic, smooth continuous pacing, no stutter, no 2x. This is a good
state, not a consolation prize.

60 fps runs and is stable, but camera and traffic *feel* faster — see Phase 3.

Env switches: `MCLA_FPS_CAP`, `MCLA_MAX_FRAME_MS`, `MCLA_VSYNC`,
`MCLA_NO_TIMER_RES`, `MCLA_PRESENT_INTERVAL`, `MCLA_TIMING_LOG`, `REX_LOG_LEVEL`.

---

## DONE

### Phase 0 — baseline and tooling ✔

- [x] Patches moved out of `generated/` into `[[midasm_hook]]` entries in
      `midnightclub_config.toml` + `src/midnightclub_hooks.cpp`. Survives
      `rexglue codegen`. Verified by regenerating and rebuilding.
      Hook signature: only the registers named in `registers`, by reference,
      C++ linkage — *not* `extern "C"`, *not* `(ctx, base)`.
      Pre-migration backup in `.patch_backup/`.
- [x] Post-migration equivalence confirmed.
- [x] Frame-time instrumentation: per-second summary + 1 ms histogram every
      10 s, one file per run (`logs/timing_<date>_<time>_cap<N>.log`).
- [x] Effective-config dump at startup (`logs/effective_config.txt`) with
      per-flag `ok`/`FAIL` per phase.

### Phase 1 — see the actual config ✔

- [x] Dump effective cvar values. Immediately found that the entire GPU config
      had never been applied.

### Phase 2 — frame pacing ✔ SOLVED

Frame times were quantized to a **15.625 ms grid = 1/64 s = Windows default
timer granularity**. Not the display, not the guest vblank rate — sweeping
`video_mode_refresh_rate` across 30/60/120/144 did not move the grid.

| timer | vsync | on-grid | fps | |
|-------|-------|---------|-----|---|
| coarse| on    | 95%     | 37.2 | original state |
| 1 ms  | on    | 62%     | 40.6 | regrids to true 60 Hz |
| coarse| off   | 93%     | 40.7 | grid survives — vsync was never the cause |
| 1 ms  | off   | **34%** | **48.4** | free-running, continuous |

- [x] Both changes required; ~30% throughput gain; now the default.

### Phase 2b — frame limiter ✔

- [x] Time-based limiter (`MCLA_FPS_CAP`), wall-clock deadline with coarse
      sleep + spin tail. Verified it does not reintroduce quantization.
- [x] Established the guest present-interval field cannot do this job — it only
      means anything to DXGI when vsync is on. `MCLA_PRESENT_INTERVAL=orig` also
      desynchronised the renderer's alternate-frame work (shadow flicker).
      **Do not use it.**

### Phase 3a — the 2x speed bug ✔ SOLVED, then verified absent

- [x] Original cause: the fixed-timestep path in `sub_821BDA90`. Fixed by the
      `MCLAUseRealDelta` hook at `0x821BDB08`, which keeps the reset guard
      (unlike Xenia's patch) while bypassing the 30 Hz paths.
- [x] **Verified by direct measurement that time advancement is now frame-rate
      correct.** Summed the delta actually used across every substep-loop
      iteration: 2.00x at the 30 cap and 2.00x at the 60 cap — *identical*.
      (The 2.00x itself is a metric artifact: the loop makes 3 passes per frame,
      two substeps of `dt/2` plus one full-`dt` pass, and summing all three
      gives `2*dt` by construction. The console ran the same loop. What matters
      is that the value does not change with frame rate.)

Four hypotheses were raised and killed by measurement along the way. Recording
them so they are not revisited:

| hypothesis | how it died |
|---|---|
| Fixed substeps per frame | `sub_821BD910` divides `dt` by the substep count correctly |
| Wrong global frame delta | `flt_827D7508` ratio is 1.00 at both 30 and 60 fps |
| Substep loop double-counts | `r24 == 2` constantly at *both* rates |
| Per-frame input integration | Steering effectiveness and cornering radius are unchanged at 60 |

### Tooling — IDA database ✔

- [x] Rebuilt with the idaxex fork at https://github.com/SaveEditors/idaxex
      (upstream emoose is 9.3-only). Imagebase `0x82000000`, 18,861 strings,
      full `.text` and function table.
- [x] `E:\MCLA\IDA\apply_rexglue_functions.py` — 30,029 function entries from
      `generated/midnightclub_init.cpp` so IDA and the recomp agree on
      boundaries.
- [x] XbSymbolDatabase ruled out (targets original Xbox XBE, not 360 XEX).
- [x] `mc4_xenon_final.pdb` unobtainable. Not a blocker.

---

## Established facts (do not re-litigate)

| Finding | Status |
|---|---|
| `0x821BDB08` is `bne cr6,0x821BDBC8` — where the 60 FPS patch belongs, not `0x821BDB68` | Confirmed in IDA |
| `[r3+56]` is a one-shot "timer was reset" flag; skipping it unconditionally caused the audio blowout | Confirmed |
| `0x82419AA0` (`li r11,2`) is a PM4 **present-interval** field, not delta time | `sub_824199B0` builds GPU packets |
| Guest timebase is **49,875,000 Hz**, not 50 MHz | `flt_82011110` = 2.00504e-8 |
| `f1` arg to `sub_821BDA90` is a **forced-delta override**; main loop passes -1.0 = "use measured" | `flt_82003770` = -1.0 |
| Main frame function is **`sub_822C1FA8`**; substep loop at `loc_822C2448` runs `r24+1` passes | `r24` constant 2 |
| `sub_821BD910(r3, enable, count)` sets `[r3+8] = dt/count`, or restores full `dt` when `enable==0` | Read from disasm |
| GPU cvars live in the xenos plugin DLL and **cannot bind from `OnPreSetup`** | All `FAIL pre` / `ok post` |
| `mount_cache` is not a registered cvar — RPF RAM caching has never been on | Absent from headers and DLL |
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

## AUDIT — 2026-08-08

Reviewed everything checked off above. Findings, all fixed unless noted.

### Bugs found in our own code

| # | Finding | Severity | Status |
|---|---|---|---|
| 1 | `MCLASubstepCount` / `MCLASubstepDelta` ran **unconditionally**, not gated on `MCLA_TIMING_LOG`. A guest-memory read plus a `Runtime::instance()` call on every substep pass (3x/frame) during normal play. | Real, shipped overhead | Fixed — gated |
| 2 | Frame limiter and frame-time bookkeeping used plain non-atomic statics, but `sub_821BDA90` has **two callers** (`sub_822C1FA8` and the reset path `sub_822611B0`) and nothing guaranteed one thread. A second thread would corrupt the schedule and sleep where it should not. | Latent race | Fixed — both bound to the first calling thread |
| 3 | `measured_dt` assumed a 1000 ms window; the report fires on the first frame at or past the boundary, so the real window is 1000-1035 ms. Biased `measured_dt` low by up to 3% — and `ratio` is derived from it. | Measurement error | Fixed — uses actual elapsed |
| 4 | Log still printed `"2x dt <-- DOUBLE"`, which we **disproved**. Anyone reading a fresh log would reach the wrong conclusion. | Misleading output | Fixed — relabelled as a rate-invariance check |
| 5 | `effective_config.txt` header said "after OnPreSetup"; it is sampled in `OnPostSetup`. | Misleading output | Fixed |
| 6 | `timeBeginPeriod(1)` never paired with `timeEndPeriod(1)`. | Hygiene | Fixed — added `OnShutdown` |
| 7 | `anisotropic_override` is an **enum index**, not a multiplier. The config asked for `"16"`, which is invalid; it read back as 3 (= 4x). `5` means 16x. | Never worked as intended | Fixed — set to `5` |
| 8 | `d3d12_pipeline_creation_threads` was silently dropped when I rewrote the config. | Silent regression of intent | Documented — left at `-1` (auto), which is a better choice than the original hardcoded 8, but now recorded as a decision |
| 9 | `gpu_allow_invalid_fetch_constants=true` is now applied **for the first time ever** (it never bound before). Enabling it changes how invalid texture fetches render — an unvalidated behaviour change, and a candidate for the one-off white HUD. | Unvalidated | Made switchable via `MCLA_ALLOW_INVALID_FETCH` |

### CONFIRMED BUG, now fixed: our patch froze the time accumulators

Measured. `[r3+20]` and `[r3+24]` advanced exactly once — to 0.0333, one 30 fps
frame, on the single reset frame that takes the `loc_821BDBC8` path, which does
accumulate — and then sat frozen for the entire session:

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
— at 60 fps that condition is true every frame, and if the game honours the flag
it may throttle logic to 30 Hz.

`loc_821BDB90` (taken when `[r3+58]` or `[r3+60]` are set) still does its own
fixed-step overwrite. Original behaviour for that state, no evidence it is
wrong, deliberately left alone.

- [x] **Re-tested — and it regressed 60 fps to a real, measured 2x.**
      Narrowing the bypass let `loc_821BDB90` run again, and that path overwrites
      `[r3+8]` with the fixed 33.3 ms step regardless of real frame time:
      SIM RATE 4.00x and ACCUM +1.97/s at the 60 cap. The risk was noted in this
      file as "deliberately left alone" — identified and then not acted on.

### Fixed: `loc_821BDB90` now uses the measured delta

New hook `MCLAFixedStepPath` at `0x821BDB90`, `after_instruction`, registers
`["r3", "f11"]`. It replaces the freshly-loaded fixed step in `f11` with the
measured unscaled delta from `[r3+88]` (still intact at that point — written at
`0x821BDAF8`, not clobbered until `0x821BDB9C`). Every instruction downstream
then does the right thing unaided: `f0` becomes measured*timescale, both stores
write measured values, and the accumulators advance by the measured delta.

Chosen over bypassing the block, which would have lost its accumulator updates —
the exact mistake that froze `[r3+20]` in the first place.

**Verified:**

| | SIM RATE | ACCUM | `loc_821BDB90` hits |
|---|---|---|---|
| 30 cap | 2.00-2.01x | +1.00/s | 30-31/sec |
| 60 cap | 2.00-2.01x | +1.01/s | 57-61/sec |

Rate-invariant, accumulators tracking real time, and 60 fps no longer feels 2x.

**Key discovery: `loc_821BDB90` is taken EVERY frame**, not occasionally —
`[r3+58]` or `[r3+60]` is always set during gameplay. So the original wide
bypass at `0x821BDB08` was accidentally correct about speed: it worked by
skipping the main fixed-step path, not for any of the reasons attached to it at
the time. Float registers work in a hook's `registers` list, passed as
`PPCRegister&` with the value in `.f64`.

### Original concern (superseded by the above)

`MCLAUseRealDelta` jumps to `loc_821BDC34`, which only increments the frame
counters at `[r3+44]`/`[r3+48]`. That **bypasses `loc_821BDB10..loc_821BDB58`,
where `[r3+20]` and `[r3+24]` — running accumulated-time totals — are advanced.**

Xenia's patch skips them too, so this is not a deviation from the reference. But
"Xenia does it as well" is not evidence that nothing reads them. If a subsystem
uses `[r3+20]` as its clock, we have frozen that clock — and the BIK movies are
broken in a way that is *independent of frame rate*, which is exactly what a
frozen or garbage time source looks like.

The timer object is at `0x827D7500`, so the struct offsets are absolute
addresses. Confirmed by `[r3+8]` landing on `flt_827D7508` (the published frame
delta) and `[r3+84]` on `flt_827D7554` (initialised to 1.0 = timescale).

- [x] Added `ACCUM` line to `timing.log` sampling `[r3+20]`, `[r3+24]` and the
      timescale, with a `<-- FROZEN` marker.
- [ ] **Run and check.** If frozen, this is very likely the BIK root cause and
      the fix is to keep the accumulator update while still skipping the
      fixed-timestep overwrite.

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

### Phase 3 — per-frame smoothing constants (needed for 60 fps)

Time advancement is correct. What remains is subsystems that update by a
**per-frame constant** instead of by `dt`. Signature:
`current += (target - current) * k` applied once per frame — at double the frame
rate it settles twice as fast, while every integral over time stays correct.
This is invisible to any total-simulated-time measurement, which is why four
measurements found nothing.

Matches the symptoms exactly: top speed, acceleration and cornering radius
unchanged; camera snap, perceived turn aggressiveness and traffic motion faster.

- [ ] Find the camera update and its smoothing constant (most perceptually
      dominant, clearest signature).
- [ ] Fix pattern: replace constant `k` with `1 - pow(1 - k, dt * 30)`, or for
      small `k` the cheaper `k * dt * 30`. 30 = the design point.
- [ ] Then traffic, then sweep for others.
- [ ] This is an audit, not a single fix. Each item is small, local and
      independently verifiable, but the count is unknown up front.

### Phase 3b — intro BIK movies play too fast

- [x] Ruled out: present interval (no change, plus shadow flicker).
- [x] Ruled out: frame rate. Still fast at hard 30, 45 and 60 fps caps — speed
      is *independent* of frame rate, unlike everything else. A separate defect.
- [ ] Find the movie player's timing path.
- [ ] Note: the engine supports forced-delta injection via the `f1` argument to
      `sub_821BDA90`. If the movie player uses a second timer context, that is a
      likely mechanism and a likely fix point.

### Phase 4 — city slowdown

Still drops to 22-25 fps in dense areas, so even the 30 fps cap is not held
there. Matters regardless of whether 60 fps is ever reached.

Per frame from trace logs: ~72 coherency ops, ~50 resolves, ~68 texture uploads,
several 3.7-7.4 MB render targets. That is emulation-layer cost and worth
measuring before accepting it as fixed.

- [ ] Instrument a coarse per-frame split: guest main thread vs GPU/texture cache.
- [ ] Then one knob per run, measuring each:
  - [ ] `texture_cache_memory_limit_render_to_texture` (24 MB default, 24 GB VRAM)
  - [ ] `texture_cache_memory_limit_soft` / `_hard`
  - [ ] `render_target_path_d3d12` (ROV available per the D3D12 feature log)
  - [ ] `d3d12_tiled_shared_memory`
- [ ] Decide honestly: if cost is dominated by recompiled guest code, stop
      chasing fps and make the drops smooth instead.

### Phase 5 — hygiene

- [ ] `.loc` dummies belong at `E:\MCLA\MCLA_Game_Files\mc4\art\city\` — where
      the `t:` mount resolves. Copies in `midnightclub\cache\` and
      `user_data\Partition1\` are never consulted. Startup-only, low impact.
- [ ] Rewrite `recompilation_technical_rundown.md`. It states the wrong
      instruction at `0x821BDB08`, describes `0x82419AA0` as a delta-time patch,
      claims `mount_cache` and the `.loc` fix work, and says the timebase is
      50 MHz. It will mislead anyone who reads it.
- [ ] Remove or gate the instrumentation-only hooks (`MCLASubstepCount`,
      `MCLASubstepDelta`) once Phase 3 is done — they cost a guest-memory read
      per substep iteration.

### Noted, not chased

Two non-reproducible startup glitches: an audio blowout (once, under a
now-removed no-clamp config) and a white HUD (once, did not recur). Recorded in
case a pattern emerges.

### Traffic / NPC jitter

Largely resolved as a side effect of Phase 2 — it was a symptom of the dropped
ticks. Whatever remains at 60 fps is Phase 3.
