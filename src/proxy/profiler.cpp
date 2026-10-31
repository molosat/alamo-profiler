#include "profiler.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <mutex>

#include "memory_sampler.h"
#include "gpu_timer.h"
#include "deferred_log.h"

namespace profiler {
namespace {

Config g_cfg;
std::wstring g_dll_dir;
std::wstring g_stem;  // <output_dir>\<timestamp>, shared by the session files
bool g_init = false;
thread_local unsigned g_present_depth = 0;
HANDLE g_log_wake = nullptr;
LARGE_INTEGER g_qpc_freq;
LARGE_INTEGER g_qpc_base;
perf::FrameStats g_stats;
int g_dll_zone = -1;
thread_local double g_pending_self_ms = 0.0;
FILE* g_log = nullptr;
FILE* g_csv = nullptr;
bool g_marker_down = false;
std::mutex g_log_mutex;
std::mutex g_frame_mutex;
std::atomic<bool> g_stopping{false};
std::atomic_flag g_shutdown_once = ATOMIC_FLAG_INIT;
DeferredLog* g_diagnostics = new DeferredLog;
std::atomic<bool> g_defer_logs{false};
// Caller owns g_log_mutex; Present-time producers only enqueue lines.
void WriteLogLine(const char* line) {
    if (!g_log) return;
    fprintf(g_log, "%s\n", line);
}

void FormatLogLine(char (&line)[1024], const char* fmt, va_list ap) {
    const int prefix = snprintf(line, sizeof line, "[%10.3f] ", NowMs());
    if (prefix < 0 || static_cast<size_t>(prefix) >= sizeof line) { line[0] = '\0'; return; }
    vsnprintf(line + prefix, sizeof line - static_cast<size_t>(prefix), fmt, ap);
}

void DrainLogLines() {
    g_diagnostics->Drain(WriteLogLine);
    static uint64_t reported = 0;  // Serialized by g_log_mutex.
    const auto dropped = g_diagnostics->Dropped();
    if (dropped != reported) {
        char line[128];
        snprintf(line, sizeof line, "[%10.3f] WARNING: dropped %llu diagnostic log lines", NowMs(),
                 static_cast<unsigned long long>(dropped - reported));
        WriteLogLine(line);
        reported = dropped;
    }
    if (g_log) fflush(g_log);
}

DWORD WINAPI RunLogWriter(void*) {
    for (;;) {
        WaitForSingleObject(g_log_wake, 50);
        if (g_stopping.load(std::memory_order_acquire)) return 0;
        std::lock_guard<std::mutex> lock(g_log_mutex);
        if (!g_stopping.load(std::memory_order_acquire)) DrainLogLines();
    }
}

void StartLogWriter() {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           reinterpret_cast<LPCWSTR>(&StartLogWriter), &module)) {
        Log("ERROR: cannot pin session writer module (%lu)", GetLastError());
        return;
    }
    Log("session writer: module pinned for process lifetime");
    g_log_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HANDLE thread = g_log_wake ? CreateThread(nullptr, 0, RunLogWriter, nullptr, 0, nullptr) : nullptr;
    if (thread) CloseHandle(thread);
    else Log("ERROR: cannot start session writer (%lu)", GetLastError());
}

std::wstring ReadIni(const std::wstring& ini, const wchar_t* key, const wchar_t* def) {
    wchar_t buf[MAX_PATH];
    GetPrivateProfileStringW(L"alamo_profiler", key, def, buf, MAX_PATH, ini.c_str());
    return buf;
}

// GetPrivateProfileInt is decimal-only; this also takes 0x hex (key codes).
int ReadIniInt(const std::wstring& ini, const wchar_t* key, int def) {
    std::wstring s = ReadIni(ini, key, L"");
    if (s.empty()) return def;
    wchar_t* end = nullptr;
    long v = wcstol(s.c_str(), &end, 0);
    return end != s.c_str() ? static_cast<int>(v) : def;
}

std::wstring Absolute(const std::wstring& p) {
    if (p.size() > 1 && (p[1] == L':' || (p[0] == L'\\' && p[1] == L'\\'))) return p;
    return g_dll_dir + L"\\" + p;
}

}  // namespace

void Init(HMODULE self) {
    if (g_init) return;
    g_init = true;
    QueryPerformanceFrequency(&g_qpc_freq);
    QueryPerformanceCounter(&g_qpc_base);

    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(self, path, MAX_PATH);
    g_dll_dir.assign(path, n);
    g_dll_dir.erase(g_dll_dir.find_last_of(L"\\/"));

    std::wstring ini = g_dll_dir + L"\\alamo_profiler.ini";
    g_cfg.enabled = ReadIniInt(ini, L"Enabled", 1) != 0;
    g_cfg.frames_csv = ReadIniInt(ini, L"FramesCsv", 1) != 0;
    g_cfg.gpu_timing = ReadIniInt(ini, L"GpuTiming", 1) != 0;
    g_cfg.draw_timing = ReadIniInt(ini, L"DrawTiming", 1) != 0;
    g_cfg.flush_every = ReadIniInt(ini, L"FlushEvery", 120);
    g_cfg.memory_every = ReadIniInt(ini, L"MemoryEvery", 60);
    if (g_cfg.flush_every < 1) g_cfg.flush_every = 1;
    g_cfg.marker_vk = ReadIniInt(ini, L"MarkerKey", VK_F11);
    g_cfg.output_dir = Absolute(ReadIni(ini, L"OutputDir", L"perf_logs"));
    std::wstring chain = ReadIni(ini, L"ChainLoad", L"");
    if (!chain.empty()) g_cfg.chain_load = Absolute(chain);
    if (!g_cfg.enabled) return;
    if (g_cfg.frames_csv) g_dll_zone = g_stats.RegisterZone("dll");
    CreateDirectoryW(g_cfg.output_dir.c_str(), nullptr);
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t stamp[32];
    swprintf(stamp, 32, L"%04u%02u%02u_%02u%02u%02u", st.wYear, st.wMonth, st.wDay,
             st.wHour, st.wMinute, st.wSecond);
    g_stem = g_cfg.output_dir + L"\\" + stamp;
    g_log = _wfopen((g_stem + L"_session.txt").c_str(), L"w");
    Log("Alamo Profiler session %ls", stamp);
    Log("config: gpu_timing=%d draw_timing=%d flush_every=%d memory_every=%d marker_vk=0x%X",
        g_cfg.gpu_timing, g_cfg.draw_timing, g_cfg.flush_every, g_cfg.memory_every, g_cfg.marker_vk);
    Log("qpc frequency: %lld Hz", static_cast<long long>(g_qpc_freq.QuadPart));

    StartLogWriter();
    g_defer_logs.store(true, std::memory_order_release);
}

const Config& Cfg() { return g_cfg; }

double NowMs() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return static_cast<double>(now.QuadPart - g_qpc_base.QuadPart) * 1000.0 /
           static_cast<double>(g_qpc_freq.QuadPart);
}

perf::FrameStats& Stats() { return g_stats; }
bool Stopping() { return g_stopping.load(std::memory_order_acquire); }

void EnterPresent() { ++g_present_depth; }
void LeavePresent() { --g_present_depth; }
bool InsidePresent() { return g_present_depth!=0; }
void Log(const char* fmt, ...) {
    if (g_stopping.load(std::memory_order_acquire)) return;
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    FormatLogLine(line, fmt, ap);
    va_end(ap);
    if (g_defer_logs.load(std::memory_order_acquire)) {
        g_diagnostics->Push(line);
        return;
    }
    std::lock_guard<std::mutex> lock(g_log_mutex);
    WriteLogLine(line);
    if (g_log) fflush(g_log);
}

void OpenFramesCsv() {
    if (g_csv || !g_cfg.enabled || !g_cfg.frames_csv) return;
    std::wstring path = g_stem + L"_frames.csv";
    g_csv = _wfopen(path.c_str(), L"w");
    if (!g_csv) {
        Log("ERROR: cannot open %ls", path.c_str());
        return;
    }
    g_stats.WriteHeader(g_csv);
    Log("frames csv: %ls", path.c_str());
    if (g_cfg.memory_every > 0) {
        const auto memory_path = g_stem + L"_memory.csv";
        if (memory_sampler::Start(memory_path)) Log("memory csv: %ls", memory_path.c_str());
        else Log("ERROR: cannot start memory sampler for %ls", memory_path.c_str());
    }
}

void AddSelfTime(double ms) {
    if (g_cfg.frames_csv) g_pending_self_ms += ms;
}

void OnFrameEnd(double present_start_ms, double present_end_ms) {
    const double self_start = g_cfg.frames_csv ? NowMs() : 0.0;
    if (g_stopping.load(std::memory_order_acquire)) return;
    std::unique_lock<std::mutex> frame_lock(g_frame_mutex, std::try_to_lock);
    if (!frame_lock.owns_lock() || g_stopping.load(std::memory_order_acquire)) return;
    if (g_cfg.marker_vk) {
        bool down = (GetAsyncKeyState(g_cfg.marker_vk) & 0x8000) != 0;
        if (down && !g_marker_down) {
            g_stats.AddMark();
            Log("mark at frame %llu",
                static_cast<unsigned long long>(g_stats.FramesClosed() + 1));
        }
        g_marker_down = down;
    }
    bool first_present = !g_stats.Started();
    if (g_cfg.frames_csv) {
        g_stats.AddZoneTime(g_dll_zone, g_pending_self_ms);
        g_pending_self_ms = 0.0;
    }
    uint64_t frame = g_stats.EndFrame(present_start_ms, present_end_ms);
    if (first_present) {
        Log("csv clock origin: first Present at profiler %.3f ms", g_stats.OriginMs());
    }
    gpu::TagLastFrame(frame);
    gpu::Poll();
    if (g_csv && frame && g_cfg.memory_every > 0 &&
        frame % static_cast<uint64_t>(g_cfg.memory_every) == 0)
        memory_sampler::Request(frame, present_end_ms - g_stats.OriginMs());
    if (frame % static_cast<uint64_t>(g_cfg.flush_every) == 0) {
        g_stats.Drain(g_csv, false);
        if (g_csv) fflush(g_csv);
    }
    // The frame boundary precedes this bookkeeping, so charge the next frame.
    if (g_cfg.frames_csv) g_stats.AddZoneTime(g_dll_zone, NowMs() - self_start);
}

void FlushCsv() {
    if (g_stopping.load(std::memory_order_acquire)) return;
    std::unique_lock<std::mutex> frame_lock(g_frame_mutex, std::try_to_lock);
    if (!frame_lock.owns_lock()) return;
    g_stats.Drain(g_csv, true);
    if (g_csv) fflush(g_csv);
}

void Shutdown() {
    if (g_shutdown_once.test_and_set(std::memory_order_acq_rel)) return;
    g_stopping.store(true, std::memory_order_release);
    memory_sampler::RequestStop();
    if (g_log_wake) SetEvent(g_log_wake);
    // ExitProcess may have killed workers holding locks. Only flush when
    // no frame owns the accumulator; never wait for workers during teardown.
    std::unique_lock<std::mutex> frame_lock(g_frame_mutex, std::try_to_lock);
    if (frame_lock.owns_lock()) {
        g_stats.Drain(g_csv, true);
        if (g_csv) {
            fclose(g_csv);
            g_csv = nullptr;
        }
    }
    // ExitProcess may have killed a thread while it held the log lock.
    std::unique_lock<std::mutex> lock(g_log_mutex, std::try_to_lock);
    if (!lock.owns_lock()) return;
    if (g_log) {
        DrainLogLines();
        memory_sampler::WriteSummary(g_log);
        if (frame_lock.owns_lock())
            fprintf(g_log, "[%10.3f] shutdown after %llu frames\n", NowMs(),
                    static_cast<unsigned long long>(g_stats.FramesClosed()));
        else
            fprintf(g_log, "[%10.3f] shutdown while frame bookkeeping is busy\n", NowMs());
        fclose(g_log);
        g_log = nullptr;
    }
}


}  // namespace profiler
