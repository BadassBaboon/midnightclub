
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
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")

#include <rex/graphics/flags.h>
#include <rex/ui/flags.h>

class MidnightclubApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<MidnightclubApp>(new MidnightclubApp(ctx, "midnightclub",
        PPCImageConfig));
  }

  void OnPreSetup(rex::RuntimeConfig& config) override {
    config.gpu_plugin = "xenos";

    // 1. Internal Resolution Scaling (1 = Native Render Targets to prevent readback stalls)
    rex::cvar::SetFlagByName("resolution_scale", "1");
    rex::cvar::SetFlagByName("anisotropic_override", "16");
    rex::cvar::SetFlagByName("gpu_allow_invalid_fetch_constants", "true");

    // 2. Fix UI / Minimap White Box Flickering
    // (Note: d3d12_readback_resolve fixes the minimap now, so we can re-enable async shaders to fix stuttering!)
    rex::cvar::SetFlagByName("async_shader_compilation", "true");
    rex::cvar::SetFlagByName("mount_cache", "true"); // Cache RPF archives in RAM to fix streaming hitches

    // 3. Modern GPU & CPU Performance Optimizations (D3D12 Bindless + Multi-threading)
    rex::cvar::SetFlagByName("d3d12_bindless", "true");
    rex::cvar::SetFlagByName("d3d12_pipeline_creation_threads", "8");
    rex::cvar::SetFlagByName("d3d12_readback_resolve", "true"); // Fixes minimap/UI textures not rendering
    rex::cvar::SetFlagByName("readback_memexport_fast", "true");
    rex::cvar::SetFlagByName("d3d12_allow_variable_refresh_rate_and_tearing", "true");

    // 4. Display & Frame Presentation (V-Sync enabled to cap 60 FPS and lock 1.0x game speed)
    rex::cvar::SetFlagByName("window_width", "2560");
    rex::cvar::SetFlagByName("window_height", "1440");
    rex::cvar::SetFlagByName("video_mode_width", "2560");
    rex::cvar::SetFlagByName("video_mode_height", "1440");
    rex::cvar::SetFlagByName("video_mode_refresh_rate", "60");
    rex::cvar::SetFlagByName("vsync", "true");
  }


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

      fprintf(crash_log, "=== abort() called — stack trace ===\n");
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

    // Register t: drive — game uses it for city/art/collision data (.loc files etc.)
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
};
