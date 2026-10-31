// Configuration, clock, session logging and the per-frame accumulator.
#pragma once

#include <windows.h>

#include <string>

#include "frame_stats.h"

namespace profiler {

struct Config {
    bool enabled = true;
    bool frames_csv = true;
    bool gpu_timing = true;
    bool draw_timing = true;
    int flush_every = 120;          // frames between CSV flushes
    int memory_every = 60;          // frames between memory samples; <1 = off
    int marker_vk = VK_F11;         // key that bumps the marks column
    std::wstring output_dir;        // default: <dll dir>\perf_logs
    std::wstring chain_load;        // d3d9 to forward to; default: system d3d9.dll
};

// Reads alamo_profiler.ini next to the DLL and opens the session log. Idempotent.
void Init(HMODULE self);
const Config& Cfg();

double NowMs();
perf::FrameStats& Stats();
bool Stopping();
// Scope the full device Present, including message pumps in the real runtime.
void EnterPresent();
void LeavePresent();
bool InsidePresent();
// Session log: human-readable notes about this run (adapter, present params,
// what got hooked, warnings). printf-style, one line per call.
void Log(const char* fmt, ...);
// Opens the frames CSV (once, on the first device) and writes its header.
void OpenFramesCsv();
// Render-thread pending time; transferred to the dll zone under the frame lock.
void AddSelfTime(double ms);
// Called from the Present hooks after the real Present returns.
void OnFrameEnd(double present_start_ms, double present_end_ms);
// The device is gone, so no GPU result can still arrive: write every closed
// frame now. Background workers pin the DLL, so DLL unload is not a
// reliable flush point.
void FlushCsv();
void Shutdown();

}  // namespace profiler
