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
#include <cstdio>
#include <cstdlib>

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

// Frame-time instrumentation. Off unless MCLA_TIMING_LOG=1. Accumulates into
// counters and touches the filesystem once per second — never from inside a
// frame that is already late.
void RecordFrameTime() {
  static const bool enabled = [] {
    const char* e = std::getenv("MCLA_TIMING_LOG");
    return e && *e == '1';
  }();
  if (!enabled) return;

  static std::FILE* log = std::fopen("logs/timing.log", "w");
  static uint64_t last = 0, frames = 0, last_report = 0;
  static uint32_t spikes[4] = {};

  uint64_t now = std::chrono::duration_cast<std::chrono::microseconds>(
                     std::chrono::steady_clock::now().time_since_epoch())
                     .count();

  if (last != 0) {
    uint64_t d = now - last;
    if (d > 100000) spikes[3]++;
    else if (d > 50000) spikes[2]++;
    else if (d > 33000) spikes[1]++;
    else if (d > 20000) spikes[0]++;
  }
  last = now;

  frames++;
  if (last_report == 0) last_report = now;
  if (now - last_report >= 1000000 && log) {
    std::fprintf(log, "fps=%llu  spikes: >20ms=%u >33ms=%u >50ms=%u >100ms=%u\n",
                 static_cast<unsigned long long>(frames), spikes[0], spikes[1],
                 spikes[2], spikes[3]);
    std::fflush(log);
    frames = 0;
    last_report = now;
    spikes[0] = spikes[1] = spikes[2] = spikes[3] = 0;
  }
}

}  // namespace

// Signatures must match what rexglue emits: only the registers named in the
// hook's `registers` list, passed by reference, with C++ linkage. See the
// `extern` declarations codegen writes above each call site.

// 0x821BDAB0, after `subf r8,r10,r11`.
void MCLAFrameDelta(PPCRegister& r8) {
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
void MCLAPresentInterval(PPCRegister& r11) {
  r11.s64 = 1;
}
