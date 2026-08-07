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

#include <chrono>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>

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
  static uint64_t start = 0, last_hist = 0;
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
  if (start == 0) { start = now; last_hist = now; }

  bool wrote = false;

  frames++;
  if (last_report == 0) last_report = now;
  if (now - last_report >= 1000000 && log) {
    wrote = true;
    std::fprintf(log, "[%6.1fs] fps=%llu  spikes: >20ms=%u >33ms=%u >50ms=%u >100ms=%u\n",
                 (now - start) / 1e6, static_cast<unsigned long long>(frames),
                 spikes[0], spikes[1], spikes[2], spikes[3]);
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

// 0x821BDB08, before `bne cr6,0x821BDBC8`.
// True  -> jump to 0x821BDC34, keeping the measured delta.
// False -> original branch runs, so reset frames still reach loc_821BDBC8 and
//          get a synthetic delta rather than a garbage one.
bool MCLAUseRealDelta(PPCCRRegister& cr6) {
  return cr6.eq;
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
