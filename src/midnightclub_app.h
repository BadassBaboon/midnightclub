
// midnightclub - ReXGlue Recompiled Project
//
// This file is yours to edit. 'rexglue migrate' will NOT overwrite it.
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <rex/rex_app.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>
#include <rex/ppc/context.h>
#include <rex/cvar.h>
#include <cstdio>
#include <csignal>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <filesystem>
#include <string>
#include <vector>
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#include <timeapi.h>
#pragma comment(lib, "winmm.lib")

#include <rex/graphics/flags.h>
#include <rex/logging/api.h>
#include <rex/ui/flags.h>

class MidnightclubApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<MidnightclubApp>(new MidnightclubApp(ctx, "midnightclub",
        PPCImageConfig));
  }

  // Record of every SetFlag attempt, so failures are visible in a file rather
  // than on stderr - which is invisible under `Start-Process`.
  static inline std::vector<std::string>& FlagLog() {
    static std::vector<std::string> log;
    return log;
  }

  // SetFlagByName returns false for names that were never registered. Critically,
  // cvars defined in the GPU plugin DLL are NOT registered until the plugin is
  // loaded, which happens during Runtime::Setup() - i.e. AFTER OnPreSetup. So
  // GPU flags set from OnPreSetup silently fail to bind.
  static void SetFlag(const char* phase, const char* name, const char* value) {
    bool ok = rex::cvar::SetFlagByName(name, value);
    FlagLog().push_back(std::string(ok ? "  ok   " : "  FAIL ") + phase + "  " +
                        name + " = " + value);
  }

  // GPU-plugin cvars. These MUST be applied from OnPostSetup, not OnPreSetup.
  //
  // Everything declared in rex/graphics/flags.h lives in the xenos plugin DLL,
  // which Runtime::Setup() loads *after* OnPreSetup returns. Setting them any
  // earlier does nothing at all: measured across four runs, every one of these
  // reported `FAIL pre` / `ok post`. This silently discarded the entire GPU
  // configuration for the lifetime of the project - most consequentially vsync,
  // which stayed at its default of `true` while the config said `false`.
  //
  // Window/display cvars (rex/ui/flags.h) live in rexruntime and DO bind from
  // OnPreSetup, which is why those were the only settings that ever worked.
  void ApplyGpuFlags(const char* phase) {
    // Internal render resolution multiplier. The guest renders 1280x720 internally,
    // and scale=2 would give 2560x1440 eDRAM-emulated render targets before
    // upscaling to the window - which is supersampling, NOT the same as rendering
    // natively at the window resolution.
    //
    // CONFIRMED BROKEN at scale=2 (2026-08-13):
    //   - Race-start showcase cameras pick wrong/random world positions at higher
    //     altitude. Sub_8231D3A8's frustum culling uses flt_828608F0/F4/F8 (view
    //     frustum plane normals derived from the projection matrix). A 2x larger
    //     eDRAM surface changes the projection aspect fed into those normals,
    //     causing the in-frustum threshold check (dot > 0.7) to produce wrong
    //     results - the showcase picks opponents that are behind the camera.
    //   - Cars spawn tilted 45 deg floating above the grid. The grid spawner
    //     (sub_82264590 / RaceGrid_SetPosition) reads a world transform that
    //     rexglue apparently derives partly from the same render-target dimensions.
    //     With scale=2 those transforms contain garbage rotation.
    //
    // The window is already set to 2560x1440 by the display cvars below.
    // At scale=1 the emulator stretches the 1280x720 guest framebuffer to fill
    // the 2560x1440 window using its own bilinear upscaler - exactly the same
    // visual path that was used in every working Xenia build.
    //
    // If a future rexglue version fixes the projection-matrix bleed-through, or
    // if sub_8231D3A8 can be patched to use a fixed aspect, revisit this.
    // Until then: leave at 1 and do NOT change without testing race starts.
    const char* res = getenv("MCLA_RESOLUTION_SCALE");
    SetFlag(phase, "resolution_scale", (res && *res) ? res : "1");

    // eDRAM emulation path: "d3d12_rov" or "d3d12_rtv". Unset = auto.
    //
    // ROV (rasterizer-ordered views) emulates the Xbox 360's eDRAM with exact
    // per-pixel ordering, so blending, alpha and depth behave as the hardware
    // did. RTV approximates it with real render targets, and approximation
    // error in blending is the classic cause of wrong-coloured lighting - the
    // green/red glitching seen on cars here. The startup log confirms the
    // adapter supports ROVs ("Rasterizer-ordered views: yes").
    //
    // This is one of the few rendering levers rexglue actually exposes: the
    // xenos plugin ships as a prebuilt DLL, so upstream Xenia fork fixes cannot
    // be ported into it.
    if (const char* rt = getenv("MCLA_RT_PATH"); rt && *rt) {
      SetFlag(phase, "render_target_path_d3d12", rt);
    }

    // anisotropic_override is an ENUM INDEX, not a multiplier:
    //   -1 = no override, 0 = off, 1 = 1x, 2 = 2x, 3 = 4x, 4 = 8x, 5 = 16x
    // The original config asked for "16", which is not a valid index - it read
    // back as 3 (4x). 5 is the value that actually means 16x.
    SetFlag(phase, "anisotropic_override", "5");

    SetFlag(phase, "async_shader_compilation", "true");
    SetFlag(phase, "d3d12_bindless", "true");
    SetFlag(phase, "d3d12_readback_resolve", "false");
    SetFlag(phase, "readback_memexport_fast", "true");

    // d3d12_pipeline_creation_threads: deliberately left at its default of -1
    // (auto). The original config asked for 8, but that call never bound, so 8
    // has never actually run. Auto sizes to the host CPU, which is a better
    // choice than a hardcoded 8 - recording this as a decision rather than
    // silently dropping the setting.

    // gpu_allow_invalid_fetch_constants: the GPU log warns about texture fetch
    // constants with an "invalid" type and suggests this flag. It defaults to
    // false and, because of the OnPreSetup binding bug, has never been enabled
    // until now. Enabling it changes how those invalid fetches render, so it is
    // an unvalidated behaviour change - a candidate for the one-off white HUD
    // seen once during testing. Switchable so it can be A/B'd if that recurs.
    const char* fetch = getenv("MCLA_ALLOW_INVALID_FETCH");
    SetFlag(phase, "gpu_allow_invalid_fetch_constants",
            (fetch && *fetch) ? fetch : "true");

    // BadassBaboon's Recomp Adjustments: Increased texture cache limits (1536MB soft / 2048MB hard / 64MB RTT)
    // to prevent premature eviction of CTX1 normal maps and sector texture dictionaries during high-speed driving.
    const char* tex_soft = getenv("MCLA_TEX_SOFT");
    SetFlag(phase, "texture_cache_memory_limit_soft", (tex_soft && *tex_soft) ? tex_soft : "1536");

    const char* tex_hard = getenv("MCLA_TEX_HARD");
    SetFlag(phase, "texture_cache_memory_limit_hard", (tex_hard && *tex_hard) ? tex_hard : "2048");

    const char* tex_rtt = getenv("MCLA_TEX_RTT");
    SetFlag(phase, "texture_cache_memory_limit_render_to_texture", (tex_rtt && *tex_rtt) ? tex_rtt : "64");

    const char* tiled = getenv("MCLA_TILED_SHARED");
    SetFlag(phase, "d3d12_tiled_shared_memory", (tiled && *tiled) ? tiled : "false");

    // BadassBaboon's Recomp Adjustments: vsync is false by default for maximum throughput (~30% higher framerate, eliminating 15.625ms quantization grid).
    const char* vs = getenv("MCLA_VSYNC");
    SetFlag(phase, "vsync", (vs && *vs) ? vs : "false");
  }

  // Pair for timeBeginPeriod(1) in OnPostSetup. Windows restores the timer
  // resolution on process exit anyway, but leaving a raised system-wide timer
  // resolution dangling is sloppy and matters if the process ever shuts down
  // without exiting.
  void OnShutdown() override {
    if (timer_res_raised_) {
      timeEndPeriod(1);
      timer_res_raised_ = false;
    }
  }

  void OnPreSetup(rex::RuntimeConfig& config) override {
    config.gpu_plugin = "xenos";

    // GPU flags are NOT set here - see ApplyGpuFlags. They cannot bind yet.

    // NOTE: "mount_cache" is not a cvar in rexglue 0.9.0 - the name appears
    // nowhere in the SDK headers or in rexruntimerd.dll, so the old call here
    // was a silent no-op and RPF archives were never cached in RAM.
    //
    // DO NOT set clear_memory_page_state=false. The SDK describes it as
    // "Refresh page-valid state from GPU-written memory at frame end. Disable
    // for minor CPU overhead reduction, but may break memory coherency."
    // Breaking coherency is what makes the minimap flicker as a white box.

    SetFlag("pre ", "d3d12_allow_variable_refresh_rate_and_tearing", "true");

    // Window / display cvars live in rexruntime, which IS loaded here, so
    // unlike the GPU-plugin flags these do bind from OnPreSetup.
    SetFlag("pre ", "window_width", "2560");
    SetFlag("pre ", "window_height", "1440");
    SetFlag("pre ", "video_mode_width", "2560");
    SetFlag("pre ", "video_mode_height", "1440");
    // Phase 2, Experiment 2.1: is the guest vblank worker the source of the
    // ~15.4 ms frame-time quantum? Sweep this without rebuilding via
    // MCLA_REFRESH_RATE. If the quantum tracks this value, the vblank worker
    // paces the game and we control it directly.
    const char* refresh = getenv("MCLA_REFRESH_RATE");
    SetFlag("pre ", "video_mode_refresh_rate", (refresh && *refresh) ? refresh : "60");

    SetFlag("pre ", "fullscreen", "true"); // Bypass DWM VBlank waiting
  }

  // Dump the *effective* value of every cvar we care about, so a run that
  // shows no behavioural change can be distinguished from a setting that never
  // applied. Two settings in this file were silently doing nothing before we
  // started checking, so this is not hypothetical.
  void DumpEffectiveConfig() {
    static const char* kWatched[] = {
        "video_mode_refresh_rate", "video_mode_width", "video_mode_height",
        "vsync", "fullscreen", "window_width", "window_height",
        "resolution_scale", "async_shader_compilation", "clear_memory_page_state",
        "d3d12_bindless", "d3d12_readback_resolve", "readback_resolve",
        "readback_memexport_fast", "d3d12_pipeline_creation_threads",
        "d3d12_allow_variable_refresh_rate_and_tearing", "d3d12_tiled_shared_memory",
        "render_target_path_d3d12", "texture_cache_memory_limit_soft",
        "texture_cache_memory_limit_hard",
        "texture_cache_memory_limit_render_to_texture",
        "anisotropic_override", "gpu_allow_invalid_fetch_constants", "log_level",
    };
    std::filesystem::create_directories("logs");
    if (FILE* f = fopen("logs/effective_config.txt", "w")) {
      fprintf(f, "=== effective cvar values (sampled in OnPostSetup, after all flags applied) ===\n");
      for (const char* name : kWatched) {
        std::string v = rex::cvar::GetFlagByName(name);
        fprintf(f, "%-46s = %s\n", name, v.empty() ? "<empty/unset>" : v.c_str());
      }
      fprintf(f, "\n=== env overrides ===\n");
      for (const char* e : {"MCLA_REFRESH_RATE", "MCLA_MAX_FRAME_MS",
                            "MCLA_TIMING_LOG", "MCLA_NO_TIMER_RES", "MCLA_VSYNC", "MCLA_PRESENT_INTERVAL", "MCLA_FPS_CAP",
                            "MCLA_ALLOW_INVALID_FETCH", "MCLA_SUBSTEPS",
                            "MCLA_RT_PATH", "MCLA_RESOLUTION_SCALE",
                            "MCLA_TEX_SOFT", "MCLA_TEX_HARD", "MCLA_TEX_RTT", "MCLA_TILED_SHARED",
                            // Gameplay / rendering knobs. These were missing from
                            // the dump, which made "did my setting apply?" -
                            // the entire reason this file exists - unanswerable
                            // for exactly the settings most likely to be wrong.
                            "MCLA_SKIP_INTRO", "MCLA_LOD_CITY_SCALE",
                            "MCLA_CAMERA_SMOOTH_SCALE",
                            "MCLA_TRAFFIC_DENSITY_SCALE", "MCLA_PED_DENSITY_SCALE",
                            "MCLA_PARKED_CAR_SCALE", "MCLA_TRAFFIC_UNSPAWN_MAX",
                            "MCLA_DISABLE_DOF", "MCLA_DISABLE_MSAA",
                            "MCLA_DISABLE_MOTION_BLUR", "MCLA_DISABLE_IMPOSTER_SHADOWS",
                            "MCLA_RESOLVE_SYMBOLS",
                            "REX_LOG_LEVEL"}) {
        const char* v = getenv(e);
        fprintf(f, "%-46s = %s\n", e, v ? v : "<not set>");
      }
      fprintf(f, "\n=== SetFlagByName results (pre = OnPreSetup, post = OnPostSetup) ===\n");
      for (const std::string& line : FlagLog()) {
        fprintf(f, "%s\n", line.c_str());
      }
      fclose(f);
    }
  }

  // NOTE ON LOG LEVEL: this build defaults to `trace` because BuildLogConfig
  // falls back to a build-type default on non-Release builds, and trace on this
  // title costs ~7,500 lines/sec of synchronous formatting and file I/O during
  // gameplay. Calling rex::SetAllLevels() from OnPostInitLogging() does NOT
  // work - verified: the [info] banner rex_app.cpp emits immediately after that
  // hook still reaches the log. Set the level with the REX_LOG_LEVEL
  // environment variable instead (BuildLogConfig honours it and rex_app passes
  // an empty cli_level, so env wins):
  //
  //     $env:REX_LOG_LEVEL = "warn"
  //


  void OnConfigurePaths(rex::PathConfig& paths) override {
    if (std::filesystem::exists("E:/MCLA/MCLA_Game_Files")) {
      paths.game_data_root = "E:/MCLA/MCLA_Game_Files";
    } else if (std::filesystem::exists("../MCLA_Game_Files")) {
      paths.game_data_root = "../MCLA_Game_Files";
    }
    paths.user_data_root = "user_data";
    paths.cache_root     = "cache";
  }

  void OnPostSetup() override {
    // PHASE 2 RESULT. Frame times were quantized to a 15.625 ms grid (= 1/64 s,
    // Windows' default timer granularity). Measured across a 2x2 matrix on a
    // fixed route, as fraction of frames landing on that grid:
    //
    //   timer coarse + vsync on   95% on-grid   37.2 fps   <- original state
    //   timer  1ms   + vsync on   62% on-grid   40.6 fps   (regrids to true 60Hz)
    //   timer coarse + vsync off  93% on-grid   40.7 fps   (grid survives)
    //   timer  1ms   + vsync off  34% on-grid   48.4 fps   <- free-running
    //
    // Both are required. Fixing the timer alone just hands pacing to real
    // vsync; disabling vsync alone leaves the timer grid intact. Together the
    // distribution goes continuous and throughput rises ~30%.
    //
    // MCLA_NO_TIMER_RES=1 restores the coarse timer for A/B comparison.
    if (const char* e = getenv("MCLA_NO_TIMER_RES"); !(e && *e == '1')) {
      timeBeginPeriod(1);
      timer_res_raised_ = true;
    }

    // Re-apply GPU-plugin cvars now that the xenos plugin is actually loaded.
    ApplyGpuFlags("post");

    DumpEffectiveConfig();

    // Install SIGABRT handler that dumps a stack trace to crash_stack.txt
    // before the process dies, so we can see what called abort().
    static auto abort_handler = [](int) {
      static FILE* crash_log =
          fopen("crash_stack.txt", "w");
      if (!crash_log) return;

      HANDLE process = GetCurrentProcess();
      SymInitialize(process, nullptr, TRUE);

      void* stack[64];
      USHORT frames = CaptureStackBackTrace(0, 64, stack, nullptr);

      char sym_buf[sizeof(SYMBOL_INFO) + MAX_SYM_NAME * sizeof(TCHAR)];
      SYMBOL_INFO* sym = reinterpret_cast<SYMBOL_INFO*>(sym_buf);
      sym->MaxNameLen = MAX_SYM_NAME;
      sym->SizeOfStruct = sizeof(SYMBOL_INFO);

      IMAGEHLP_LINE64 line{};
      line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);

      fprintf(crash_log, "=== abort() called - stack trace ===\n");
      for (USHORT i = 0; i < frames; ++i) {
        DWORD64 addr = reinterpret_cast<DWORD64>(stack[i]);
        DWORD displacement = 0;
        if (SymFromAddr(process, addr, nullptr, sym) &&
            SymGetLineFromAddr64(process, addr, &displacement, &line)) {
          fprintf(crash_log, "  #%02u  %s  (%s:%lu)\n", i, sym->Name, line.FileName,
                  line.LineNumber);
        } else if (SymFromAddr(process, addr, nullptr, sym)) {
          fprintf(crash_log, "  #%02u  %s  +0x%llX\n", i, sym->Name,
                  addr - sym->Address);
        } else {
          fprintf(crash_log, "  #%02u  0x%016llX\n", i, addr);
        }
      }
      fflush(crash_log);
    };
    signal(SIGABRT, abort_handler);

    // Register t: drive - game uses it for city/art/collision data (.loc files etc.)
    rex::Runtime::instance()->file_system()->RegisterSymbolicLink(
        "t:", "\\Device\\Harddisk0\\Partition1");

    auto* fd = rex::Runtime::instance()->function_dispatcher();
    uint8_t* base = rex::Runtime::instance()->virtual_membase();

    // Deduplicating stub logger. Each unique (addr, LR) pair is logged once
    // with the first-call register args (r3-r6). A running call count is
    // appended on each subsequent hit so we can spot hot stubs without spam.
    struct StubEntry {
      uint32_t r3, r4, r5, r6;
      std::atomic<uint32_t> count{0};
    };
    static FILE* stub_log = fopen("stubs.txt", "w");
    static std::mutex stub_mutex;
    static std::unordered_map<uint64_t, StubEntry> stub_map;

    static PPCFunc* stub = [](PPCContext& ctx, uint8_t*) noexcept {
      uint32_t addr = ctx.ctr.u32;
      uint32_t lr   = ctx.lr;
      uint64_t key  = (uint64_t(addr) << 32) | lr;

      std::lock_guard<std::mutex> lock(stub_mutex);
      auto [it, inserted] = stub_map.emplace(
          std::piecewise_construct, std::forward_as_tuple(key), std::forward_as_tuple());

      if (inserted) {
        it->second.r3 = ctx.r3.u32;
        it->second.r4 = ctx.r4.u32;
        it->second.r5 = ctx.r5.u32;
        it->second.r6 = ctx.r6.u32;
        it->second.count.store(1);
        if (stub_log) {
          fprintf(stub_log,
                  "[stub] addr=0x%08X LR=0x%08X  r3=0x%08X r4=0x%08X r5=0x%08X r6=0x%08X\n",
                  addr, lr, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
          fflush(stub_log);
        }
      } else {
        uint32_t n = it->second.count.fetch_add(1) + 1;
        // Re-log every power-of-two hits so we can see hot stubs growing.
        if ((n & (n - 1)) == 0 && stub_log) {
          fprintf(stub_log,
                  "[stub] addr=0x%08X LR=0x%08X  (x%u)\n", addr, lr, n);
          fflush(stub_log);
        }
      }
    };

    // Pass 1: scan static initializer tables (base = 0x82770000).
    static constexpr uint32_t kTables[][2] = {
        {0x82770010, 0x827713E0},
        {0x827713E4, 0x827713F0},
    };
    static constexpr uint32_t kXexBase = 0x82000000;
    static constexpr uint32_t kXexEnd  = 0x829E0000;

    for (auto& [start, end] : kTables) {
      for (uint32_t addr = start; addr < end; addr += 4) {
        uint32_t fn = __builtin_bswap32(*reinterpret_cast<uint32_t*>(base + addr));
        if (fn < kXexBase || fn >= kXexEnd) continue;
        if (!fd->GetFunction(fn)) {
          fprintf(stderr, "[stub] Missing static init fn: 0x%08X\n", fn);
          fd->SetFunction(fn, stub);
        }
      }
    }

    // Bypass disc error handler: sub_82130678 is called (via CTR) whenever
    // the game's streaming disc check fails. It unconditionally shows the
    // "Dirty Disc" error UI and terminates. Since we mount extracted files
    // instead of a real optical drive, replace it with a silent no-op so
    // execution continues past the check.
    static PPCFunc* disc_error_bypass = [](PPCContext&, uint8_t*) noexcept {};
    fd->SetFunction(0x82130678, disc_error_bypass);

    // Pass 2: walk the full XEX code region and stub every 4-byte-aligned
    // address that has no generated function. Catches indirect calls from
    // game code that the static analysis missed, logging the caller's LR.
    static constexpr uint32_t kCodeBase = 0x82130000;
    static constexpr uint32_t kCodeEnd  = 0x827CD054;
    for (uint32_t addr = kCodeBase; addr < kCodeEnd; addr += 4) {
      if (!fd->GetFunction(addr)) {
        fd->SetFunction(addr, stub);
      }
    }

  }

 private:
  bool timer_res_raised_ = false;

};
