// midnightclub - mid-asm hook implementations
//
// Declared in midnightclub_config.toml under [[midasm_hook]]. These exist so
// the frame-timing work survives `rexglue codegen`, which regenerates
// everything under generated/ and would silently discard hand-edits.
//
// Background: sub_821BDA90 is the engine's frame timer. Its r3 is a timer
// object with (fields confirmed by reading the recompiled instruction stream):
//
//   +8    float   delta, time-scaled — what the engine consumes
//   +20   float   accumulated time
//   +28   float   target fps (divisor for the fixed timestep)
//   +32   float   fixed timestep value
//   +56   u8      one-shot "timer was reset" flag
//   +64   u64     last timebase tick
//   +84   float   time scale
//   +88   float   delta, unscaled
//   +92   float   raw seconds elapsed this frame

#include <rex/chrono/clock.h>
#include <rex/hook.h>
#include <rex/ppc/context.h>
#include <rex/runtime.h>

#include <bit>
#include <cstring>

#include <chrono>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>

// Substep instrumentation, defined below with the hooks.
extern int32_t g_substep_last, g_substep_min, g_substep_max;
extern uint64_t g_substep_sum, g_substep_n;
extern double g_sim_time_sum;
extern uint64_t g_sim_iters;
extern uint64_t g_fixedstep_hits;

namespace {

// Upper bound on a single frame's delta, in guest timebase ticks.
//
// Any frame longer than this advances the world by only the cap, i.e. the sim
// runs in slow motion for that frame. The original hand-patch used 33.3 ms,
// which fired constantly — 46 ms and 61 ms were the two most common frame
// times, so the sim was running at 72% and 55% speed respectively during
// normal play.
//
// Tunable via MCLA_MAX_FRAME_MS, clamped to [16, 1000] ms. There is
// deliberately no way to disable it: it is the last line of defence against an
// unbounded delta reaching the physics and audio clocks, and removing it
// produced a very loud audio blowout during testing.
uint64_t MaxFrameTicks() {
  static const uint64_t ticks = [] {
    double ms = 125.0;
    if (const char* e = std::getenv("MCLA_MAX_FRAME_MS")) {
      double v = std::atof(e);
      if (v > 0.0) ms = v;
    }
    if (ms < 16.0) ms = 16.0;
    if (ms > 1000.0) ms = 1000.0;
    uint64_t hz = rex::chrono::Clock::guest_tick_frequency();
    if (hz == 0) hz = 50000000;
    return static_cast<uint64_t>(ms * 0.001 * static_cast<double>(hz));
  }();
  return ticks;
}

// --- Engine timing globals, read straight out of guest memory ---
//
// The main loop (sub_822C1FA8) publishes a frame delta and its reciprocal to
// two globals at 0x822C2424:
//
//   822c2404  lfs   f0, flt_828747B8(r31)   ; accumulated time
//   822c2418  fdivs f0, f0, f11             ; / (float)count
//   822c2424  stfs  f0, flt_827D7508        ; GLOBAL frame delta
//   822c2428  fdivs f0, f29, f0
//   822c242c  stfs  f0, flt_827D750C        ; GLOBAL 1/delta
//
// The vehicle physics reads the timer object's [r3+8], which sub_821BD910
// divides correctly by the substep count — and top speed, acceleration and
// cornering are all confirmed correct at 60 fps. Camera smoothing and traffic
// AI are NOT correct at 60 fps. If those read these globals instead, then a
// single wrong value here explains every remaining symptom at once.
//
// So: sample them and see whether the delta actually tracks real frame time.
// The timer object itself lives at 0x827D7500 (r27/r3 in the main loop), so the
// struct offsets map directly onto absolute addresses. This is confirmed by
// [r3+8] landing exactly on flt_827D7508, the published frame delta, and
// [r3+84] on flt_827D7554, which the main loop initialises to 1.0 (timescale).
constexpr uint32_t kGuestTimerObject  = 0x827D7500;
constexpr uint32_t kGuestFrameDelta   = 0x827D7508;  // [r3+8]
constexpr uint32_t kGuestFrameRate    = 0x827D750C;  // [r3+12]

// AUDIT CONCERN. The MCLAUseRealDelta hook jumps straight to loc_821BDC34,
// which only increments the frame counters at [r3+44]/[r3+48]. That skips the
// block at loc_821BDB10..loc_821BDB58, which is where [r3+20] and [r3+24] —
// running accumulated-time totals — are advanced.
//
// Xenia's patch skips them too, so this is not a deviation from the known-good
// reference, but "Xenia does it as well" is not evidence that nothing reads
// them. If a subsystem uses [r3+20] as its clock, we have frozen that clock.
// The intro BIK movies are broken in a way that is independent of frame rate,
// which is exactly what a frozen or garbage time source would look like.
//
// Sample them and find out.
constexpr uint32_t kGuestAccumA       = 0x827D7514;  // [r3+20]
constexpr uint32_t kGuestAccumB       = 0x827D7518;  // [r3+24]
constexpr uint32_t kGuestTimeScale    = 0x827D7554;  // [r3+84]
constexpr uint32_t kGuestAccumTime    = 0x828747B8;  // flt_828747B8
constexpr uint32_t kGuestFrameCount   = 0x828747B0;  // dword_828747B0
constexpr uint32_t kGuestSubstepFixed = 0x8201D29C;  // 0.02, the 50 Hz quantum

float ReadGuestFloat(uint32_t guest_addr) {
  const uint8_t* base = rex::Runtime::instance()->virtual_membase();
  if (!base) return 0.0f;
  uint32_t raw;
  std::memcpy(&raw, base + guest_addr, sizeof(raw));
  raw = std::byteswap(raw);  // guest memory is big-endian
  return std::bit_cast<float>(raw);
}

int32_t ReadGuestInt(uint32_t guest_addr) {
  const uint8_t* base = rex::Runtime::instance()->virtual_membase();
  if (!base) return 0;
  uint32_t raw;
  std::memcpy(&raw, base + guest_addr, sizeof(raw));
  return static_cast<int32_t>(std::byteswap(raw));
}

// How often to emit the frame-time histogram, in seconds. Each histogram
// covers only its own window, so it can be attributed to a location on the
// test route rather than smearing city and hills together.
constexpr uint64_t kHistogramWindowSec = 10;

// 1 ms buckets. Index 0..kHistBuckets-2 are whole milliseconds; the last
// bucket is everything at or above that.
constexpr int kHistBuckets = 121;

// Frame-time instrumentation. Off unless MCLA_TIMING_LOG=1. Accumulates into
// counters and touches the filesystem once per second — never from inside a
// frame that is already late.
//
// The per-second summary lines are kept unchanged so runs stay comparable with
// earlier baselines. The histogram is additive: 1 ms resolution, printed with
// zero buckets omitted, which shows directly whether frame times cluster on
// multiples of a fixed quantum or spread continuously.
void RecordFrameTime() {
  static const bool enabled = [] {
    const char* e = std::getenv("MCLA_TIMING_LOG");
    return e && *e == '1';
  }();
  if (!enabled) return;

  // Same reasoning as LimitFrameRate: every static below is non-atomic, so
  // confine the bookkeeping to one thread rather than racing.
  static const std::thread::id owner = std::this_thread::get_id();
  if (std::this_thread::get_id() != owner) return;

  // One file per run. Previously this was a fixed "logs/timing.log" opened
  // with "w", so back-to-back runs silently destroyed the earlier capture —
  // which cost us a 60 fps run that had to be repeated.
  static std::FILE* log = [] () -> std::FILE* {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char name[128];
    const char* cap = std::getenv("MCLA_FPS_CAP");
    std::snprintf(name, sizeof(name), "logs/timing_%04d%02d%02d_%02d%02d%02d_cap%s.log",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec,
                  (cap && *cap) ? cap : "none");
    return std::fopen(name, "w");
  }();
  static uint64_t last = 0, frames = 0, last_report = 0;
  static uint64_t start = 0, last_hist = 0, last_report_for_sim = 0;
  static uint32_t spikes[4] = {};
  static uint32_t hist[kHistBuckets] = {};
  static uint64_t hist_frames = 0, hist_total_us = 0;

  uint64_t now = std::chrono::duration_cast<std::chrono::microseconds>(
                     std::chrono::steady_clock::now().time_since_epoch())
                     .count();

  if (last != 0) {
    uint64_t d = now - last;
    if (d > 100000) spikes[3]++;
    else if (d > 50000) spikes[2]++;
    else if (d > 33000) spikes[1]++;
    else if (d > 20000) spikes[0]++;

    int bucket = static_cast<int>(d / 1000);
    if (bucket >= kHistBuckets) bucket = kHistBuckets - 1;
    hist[bucket]++;
    hist_frames++;
    hist_total_us += d;
  }
  last = now;
  if (start == 0) { start = now; last_hist = now; last_report_for_sim = now; }

  bool wrote = false;

  frames++;
  if (last_report == 0) last_report = now;
  if (now - last_report >= 1000000 && log) {
    wrote = true;
    // Measured wall-clock frame time this second, vs what the engine believes.
    // Use the ACTUAL elapsed interval, not an assumed 1000 ms — the report
    // fires on the first frame at or past the second boundary, so the real
    // window is typically 1000-1035 ms and assuming 1000 biased measured_dt
    // low by up to 3%.
    double window_ms = (now - last_report) / 1000.0;
    double measured_dt_ms = frames ? window_ms / frames : 0.0;
    float engine_dt = ReadGuestFloat(kGuestFrameDelta);
    float engine_fps = ReadGuestFloat(kGuestFrameRate);
    float accum = ReadGuestFloat(kGuestAccumTime);
    int32_t count = ReadGuestInt(kGuestFrameCount);

    std::fprintf(log,
                 "[%6.1fs] fps=%llu  spikes: >20ms=%u >33ms=%u >50ms=%u >100ms=%u"
                 "  | measured_dt=%.2fms  engine_dt=%.2fms (%.1f fps)  accum=%.4f count=%d  ratio=%.2f\n",
                 (now - start) / 1e6, static_cast<unsigned long long>(frames),
                 spikes[0], spikes[1], spikes[2], spikes[3],
                 measured_dt_ms, engine_dt * 1000.0f, engine_fps, accum, count,
                 (engine_dt > 0.0f) ? (measured_dt_ms / (engine_dt * 1000.0f)) : 0.0);

    // Substep count at 0x822C2434. The loop makes r24+1 passes: r24 substeps
    // of dt/r24 plus one full-dt pass. Measured as a constant 2 at every frame
    // rate tested, which is the native console behaviour — different consumers
    // take different passes, so each still sees dt per frame.
    //
    // (An earlier version of this line labelled r24 > 0 as "DOUBLE". That was
    // wrong: summing all passes gives 2*dt by construction, and the value does
    // not change with frame rate. Kept as a rate-invariance check only.)
    std::fprintf(log,
                 "           substep r24: last=%d min=%d max=%d avg=%.2f  (%d passes/frame)\n",
                 g_substep_last, g_substep_min == 0x7FFFFFFF ? -999 : g_substep_min,
                 g_substep_max == -0x7FFFFFFF ? -999 : g_substep_max,
                 g_substep_n ? double(g_substep_sum) / g_substep_n : 0.0,
                 g_substep_last >= 0 ? g_substep_last + 1 : -1);
    g_substep_min = 0x7FFFFFFF;
    g_substep_max = -0x7FFFFFFF;
    g_substep_sum = 0;
    g_substep_n = 0;

    // Rate-invariance check. Sums the delta actually used across every pass, so
    // it reads ~2.00x by construction (see above). What matters is that the
    // value is IDENTICAL at different frame caps — that is what proves time
    // advancement is frame-rate independent. A number that changes with the cap
    // would mean a real speed bug.
    double real_elapsed = (now - last_report_for_sim) / 1e6;
    std::fprintf(log,
                 "           SIM RATE: advanced %.3f s of game time in %.3f s real"
                 "  -> %.2fx (expect ~2.00 at EVERY cap)   (%llu passes)\n",
                 g_sim_time_sum, real_elapsed,
                 real_elapsed > 0.0 ? g_sim_time_sum / real_elapsed : 0.0,
                 static_cast<unsigned long long>(g_sim_iters));
    g_sim_time_sum = 0.0;
    g_sim_iters = 0;
    last_report_for_sim = now;

    // Do the accumulated-time totals still advance? Our patch bypasses the
    // block that updates them. If these are frozen, anything using them as a
    // clock is broken — a prime suspect for the BIK movies.
    static float prev_accum_a = 0.0f, prev_accum_b = 0.0f;
    float accum_a = ReadGuestFloat(kGuestAccumA);
    float accum_b = ReadGuestFloat(kGuestAccumB);
    std::fprintf(log,
                 "           ACCUM [r3+20]=%.4f (+%.4f/s)  [r3+24]=%.4f (+%.4f/s)"
                 "  timescale=%.3f  %s\n",
                 accum_a, accum_a - prev_accum_a, accum_b, accum_b - prev_accum_b,
                 ReadGuestFloat(kGuestTimeScale),
                 (accum_a - prev_accum_a) < 0.001f ? "<-- FROZEN" : "");
    std::fprintf(log, "           loc_821BDB90 fixed-step path taken %llu times\n",
                 static_cast<unsigned long long>(g_fixedstep_hits));
    g_fixedstep_hits = 0;
    prev_accum_a = accum_a;
    prev_accum_b = accum_b;
    frames = 0;
    last_report = now;
    spikes[0] = spikes[1] = spikes[2] = spikes[3] = 0;
  }

  if (now - last_hist >= kHistogramWindowSec * 1000000 && log) {
    wrote = true;
    uint32_t peak = 1;
    for (int i = 0; i < kHistBuckets; ++i)
      if (hist[i] > peak) peak = hist[i];

    std::fprintf(log,
                 "\n--- frame-time histogram | window %.1fs..%.1fs | %llu frames | mean %.2f ms ---\n",
                 (last_hist - start) / 1e6, (now - start) / 1e6,
                 static_cast<unsigned long long>(hist_frames),
                 hist_frames ? (hist_total_us / 1000.0 / hist_frames) : 0.0);

    for (int i = 0; i < kHistBuckets; ++i) {
      if (!hist[i]) continue;  // omit empty buckets so clustering is obvious
      int bar = static_cast<int>(48.0 * hist[i] / peak);
      if (bar < 1) bar = 1;
      std::fprintf(log, "%s%3d ms | %6u %.*s\n",
                   i == kHistBuckets - 1 ? ">=" : "  ", i, hist[i],
                   bar, "################################################");
    }
    std::fprintf(log, "\n");

    for (int i = 0; i < kHistBuckets; ++i) hist[i] = 0;
    hist_frames = 0;
    hist_total_us = 0;
    last_hist = now;
  }

  // Flush only on the frames that actually wrote. Flushing every frame would
  // put a blocking disk write in the hot path — the original bug this
  // instrumentation was rewritten to avoid.
  if (wrote) std::fflush(log);
}

// Time-based frame limiter.
//
// Disabling vsync removed the 15.625 ms timer grid and gained ~30% throughput,
// but it also removed the only thing that was throttling presentation at all.
// The game then free-runs at 70-100+ fps in light scenes, and MCLA has
// frame-rate-dependent logic: the intro BIK movies play roughly 2.5x too fast
// at ~75 fps.
//
// Note this is NOT fixable with the guest's present-interval field. That field
// only means anything to DXGI when vsync is on; with vsync off, Present() does
// not wait no matter what interval is requested. Restoring the guest value
// (MCLA_PRESENT_INTERVAL=orig) left the movie just as fast and additionally
// desynchronised the renderer's alternate-frame work, producing shadow flicker.
//
// So the throttle has to be ours, and it has to be time-based rather than
// vblank-based, or we reintroduce quantization. Sleep coarsely to within ~1.5 ms
// of the target (accurate now that timeBeginPeriod(1) is in force), then spin
// the remainder.
//
// MCLA_FPS_CAP: target fps, 0 or unset = uncapped.
void LimitFrameRate() {
  static const double period_us = [] {
    double fps = 0.0;
    if (const char* e = std::getenv("MCLA_FPS_CAP")) fps = std::atof(e);
    if (fps < 1.0) return 0.0;
    if (fps > 1000.0) fps = 1000.0;
    return 1000000.0 / fps;
  }();
  if (period_us <= 0.0) return;

  // sub_821BDA90 has two callers: the main loop (sub_822C1FA8) and a
  // timer-reset path (sub_822611B0), both on the same timer object. Nothing
  // guarantees they are the same thread, and `next_us` below is plain
  // non-atomic state — a second thread would both corrupt the schedule and
  // sleep somewhere it should not. Bind the limiter to the first thread that
  // reaches it and make every other thread a no-op.
  static const std::thread::id owner = std::this_thread::get_id();
  if (std::this_thread::get_id() != owner) return;

  static uint64_t next_us = 0;
  auto now_us = [] {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
  };

  uint64_t now = now_us();
  if (next_us == 0) {
    next_us = now + static_cast<uint64_t>(period_us);
    return;
  }

  if (now < next_us) {
    // Coarse sleep to within 1.5 ms, then spin. Sleep granularity is ~1 ms
    // because OnPostSetup raised the process timer resolution.
    uint64_t remaining = next_us - now;
    if (remaining > 1500) {
      std::this_thread::sleep_for(std::chrono::microseconds(remaining - 1500));
    }
    while (now_us() < next_us) {
      std::this_thread::yield();
    }
  }

  uint64_t after = now_us();
  next_us += static_cast<uint64_t>(period_us);
  // If we fell far behind (streaming stall, alt-tab), resynchronise rather than
  // sprinting to catch up on a backlog of missed frames.
  if (next_us < after) next_us = after + static_cast<uint64_t>(period_us);
}

}  // namespace

// Substep count observed at 0x822C2434 this second. Read-only.
int32_t g_substep_last = -999;
int32_t g_substep_min = 0x7FFFFFFF;
int32_t g_substep_max = -0x7FFFFFFF;
uint64_t g_substep_sum = 0, g_substep_n = 0;

// Total simulated time advanced, summed over every substep-loop iteration.
double g_sim_time_sum = 0.0;
uint64_t g_sim_iters = 0;

// These two hooks are instrumentation only. They were running unconditionally,
// which meant a guest-memory read plus a Runtime::instance() call on every
// substep iteration (3x per frame) in normal play. Gate them on the same switch
// as the rest of the instrumentation.
bool TimingLogEnabled() {
  static const bool enabled = [] {
    const char* e = std::getenv("MCLA_TIMING_LOG");
    return e && *e == '1';
  }();
  return enabled;
}

// 0x822C2478, `lfs f30, 8(r27)` — [r27+8] is the delta for this iteration,
// just set by sub_821BD910.
void MCLASubstepDelta(PPCRegister& r27) {
  if (!TimingLogEnabled()) return;
  g_sim_time_sum += ReadGuestFloat(r27.u32 + 8);
  g_sim_iters++;
}

// PHASE 3 EXPERIMENT. The substep loop makes r24+1 passes per frame, and r24 is
// a constant 2 at every frame rate — so passes/second scales directly with
// frame rate: 93/s at 30 fps, 180/s at 60 fps, a factor of 1.94.
//
// Total simulated time is unaffected (sub_821BD910 divides dt by the count), and
// every time-based measurement confirms it is correct at both rates. But the
// per-object Update() calls in the loop body go through vtable+0x68 taking only
// `this` — no dt argument — so each object fetches its own timestep. Any that
// uses a per-update constant instead runs at double rate at 60 fps, which is
// what "camera, steering, AI and traffic all feel 2x" describes.
//
// MCLA_SUBSTEPS overrides r24 so we can separate the two possibilities:
//   - symptom tracks passes/second -> the offenders update per pass
//   - symptom tracks frames/second regardless -> they update outside this loop
void MCLASubstepCount(PPCRegister& r24) {
  static const int32_t override_count = [] {
    if (const char* e = std::getenv("MCLA_SUBSTEPS")) {
      int v = std::atoi(e);
      if (v >= 0 && v <= 8) return v;
    }
    return -1;  // no override
  }();
  if (override_count >= 0) r24.s64 = override_count;

  if (!TimingLogEnabled()) return;
  int32_t v = static_cast<int32_t>(r24.s32);
  g_substep_last = v;
  if (v < g_substep_min) g_substep_min = v;
  if (v > g_substep_max) g_substep_max = v;
  g_substep_sum += static_cast<uint64_t>(v < 0 ? 0 : v);
  g_substep_n++;
}

// Signatures must match what rexglue emits: only the registers named in the
// hook's `registers` list, passed by reference, with C++ linkage. See the
// `extern` declarations codegen writes above each call site.

// 0x821BDAB0, after `subf r8,r10,r11`.
//
// The limiter runs here, after this frame's delta has been computed from the
// timebase read a few instructions above. The sleep therefore lands in the
// NEXT frame's delta, which is correct: in steady state every delta contains
// exactly one limiter sleep.
void MCLAFrameDelta(PPCRegister& r8) {
  LimitFrameRate();
  RecordFrameTime();
  const uint64_t cap = MaxFrameTicks();
  if (r8.u64 > cap) {
    r8.u64 = cap;
  }
}

// 0x821BDB58 — start of the fixed-timestep block. Unconditional jump to
// loc_821BDC34, declared in the TOML; this body only exists because codegen
// emits a call. Nothing to do here.
//
// Previously hooked at 0x821BDB08 with a conditional jump, which also bypassed
// the [r3+20] / [r3+24] accumulated-time updates. They were measured frozen at
// 0.0333 for an entire session. Moving the hook down to 0x821BDB58 skips only
// the fixed-step overwrite and lets the accumulators run again.
void MCLAUseRealDelta() {}

// How many times the loc_821BDB90 fixed-step path was taken this second.
uint64_t g_fixedstep_hits = 0;

// 0x821BDB90, immediately after `lfs f11,32(r3)` loads the FIXED timestep and
// before `fmuls f0,f11,f13` consumes it.
//
// Replace the fixed step with the measured unscaled delta, which is still sitting
// in [r3+88] at this point (written at 0x821BDAF8, not clobbered until
// 0x821BDB9C). Every instruction downstream then does the right thing without
// further intervention: f0 becomes measured*timescale, the stores to [r3+8] and
// [r3+88] write measured values, and [r3+20]/[r3+24] accumulate the measured
// delta rather than a fixed 1/30 s.
//
// Bypassing this block instead would have lost those accumulator updates — the
// exact mistake that froze [r3+20] when the hook sat at 0x821BDB08.
void MCLAFixedStepPath(PPCRegister& r3, PPCRegister& f11) {
  g_fixedstep_hits++;
  f11.f64 = static_cast<double>(ReadGuestFloat(r3.u32 + 88));
}

// 0x82419AA0, after `li r11,2`. Present interval in vblanks: 2 -> 1.
//
// This site is reached when the guest's per-context interval field
// ([r31+13596]) is 2, i.e. "present every other vblank" = 30 Hz. Neighbouring
// branches in sub_824199B0 handle 0/1 -> 1, 4 -> 3, 0x80000000 -> 0, so the
// game does vary this per context.
//
// SUSPECT for the intro BIK movies playing too fast: if the movie player paces
// playback by presents rather than by elapsed time, forcing 1 here doubles its
// rate, and with vsync off there is no longer anything throttling it.
// Set MCLA_PRESENT_INTERVAL=orig to leave the guest's value untouched.
void MCLAPresentInterval(PPCRegister& r11) {
  static const bool force = [] {
    const char* e = std::getenv("MCLA_PRESENT_INTERVAL");
    return !(e && std::string(e) == "orig");
  }();
  if (force) r11.s64 = 1;
}

// 0x822A2ED4, inside sub_822A2988 (car turning physics).
// lfs f0, 12(r20) loads the timestep. We replace the loaded value with the
// real, time-scaled frame delta to fix the 2x steering speed at 60 FPS.
void MCLATurnSpeedTimestep(PPCRegister& f0) {
  float dt = ReadGuestFloat(kGuestFrameDelta);
  if (dt > 0.0f) {
    f0.f64 = static_cast<double>(dt);
  }
}


// 0x823203D4, in sub_82320298 (likely mcPlayerCamera::Update).
// Applies the 60 FPS exponential decay formula to the camera boom interpolation 
// constant before it is passed to the generic matrix Lerp function. This correctly 
// scales the chase camera motion without poisoning cockpit animations or HUD logic.
void MCLACameraBoomSmoothing(PPCRegister& f1) {
  float dt = ReadGuestFloat(kGuestFrameDelta);
  double k = f1.f64;
  if (k > 0.0 && k < 1.0) {
    f1.f64 = 1.0 - std::pow(1.0 - k, dt * 30.0);
  }
}
