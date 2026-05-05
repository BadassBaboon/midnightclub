
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
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")

class MidnightclubApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<MidnightclubApp>(new MidnightclubApp(ctx, "midnightclub",
        PPCImageConfig));
  }

  void OnConfigurePaths(rex::PathConfig& paths) override {
    paths.game_data_root = "C:/Users/zarif/Documents/repo/midnightclub-xex/extracted/Midnight Club - Los Angeles - Complete Edition (USA, Europe)";
  }

  void OnPostSetup() override {
    // Install SIGABRT handler that dumps a stack trace to crash_stack.txt
    // before the process dies, so we can see what called abort().
    static auto abort_handler = [](int) {
      static FILE* crash_log =
          fopen("C:/Users/zarif/Documents/repo/midnightclub/crash_stack.txt", "w");
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

    // Logging no-op stub — registered for any XEX address rexGlu didn't
    // generate code for. Writes addr+LR to a file so it survives abort().
    static FILE* stub_log = fopen("C:/Users/zarif/Documents/repo/midnightclub/stubs.txt", "w");
    static PPCFunc* stub = [](PPCContext& ctx, uint8_t*) noexcept {
      if (stub_log) {
        fprintf(stub_log, "[stub] addr=0x%08X LR=0x%08X\n",
                (uint32_t)ctx.ctr.u32, (uint32_t)ctx.lr);
        fflush(stub_log);
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
