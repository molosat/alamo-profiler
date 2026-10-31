#include "memory_sampler.h"
#include "memory_sample.h"
#include "profiler.h"

#include <psapi.h>
#include <atomic>
#include <mutex>

namespace memory_sampler {
namespace {

struct Sampler {
    HANDLE file = INVALID_HANDLE_VALUE;
    HANDLE wake = nullptr;
    std::atomic<bool> stop{false};
    // Single producer, atomic fields, and an odd/even publication sequence.
    // The consumer retries; the frame thread never waits for the consumer.
    std::atomic<uint64_t> sequence{0}, frame{0};
    std::atomic<double> t_ms{0};
    std::mutex summary_mutex;
    uint64_t samples = 0, skipped = 0;
    double total_ms = 0, max_ms = 0;
};
std::atomic<Sampler*> g_sampler{nullptr};


bool WriteSample(HANDLE file, const char* buffer, size_t length) {
    if (!length) return false;
    while (length) {
        DWORD written = 0;
        if (!WriteFile(file, buffer, static_cast<DWORD>(length), &written, nullptr) || !written)
            return false;
        buffer += written;
        length -= written;
    }
    return true;
}

bool QueryRegion(uint64_t address, memory::Region* region, void*) {
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(reinterpret_cast<const void*>(static_cast<uintptr_t>(address)),
                      &info, sizeof info)) return false;
    *region = {reinterpret_cast<uintptr_t>(info.BaseAddress),
               static_cast<uint64_t>(info.RegionSize), info.State == MEM_FREE};
    return true;
}

DWORD WINAPI Run(void* context) {
    auto& sampler = *static_cast<Sampler*>(context);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    using ProcessMemoryInfo = BOOL(WINAPI*)(HANDLE, PPROCESS_MEMORY_COUNTERS, DWORD);
    const auto query = reinterpret_cast<ProcessMemoryInfo>(reinterpret_cast<void*>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "K32GetProcessMemoryInfo")));
    if (!query) profiler::Log("WARNING: K32GetProcessMemoryInfo unavailable; private bytes will be 0");
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    char buffer[1024];
    bool writing = WriteSample(sampler.file, buffer, memory::FormatHeader(buffer, sizeof buffer));
    uint64_t consumed = 0, skipped = 0;
    while (writing) {
        WaitForSingleObject(sampler.wake, INFINITE);
        if (sampler.stop.load()) break;
        uint64_t sequence = 0, frame = 0;
        double t_ms = 0;
        unsigned retries = 0;
        for (;;) {
            if (sampler.stop.load()) break;
            sequence = sampler.sequence.load();
            if (!(sequence & 1)) {
                frame = sampler.frame.load();
                t_ms = sampler.t_ms.load();
                if (sequence == sampler.sequence.load()) break;
            }
            if (retries < 16) { ++retries; YieldProcessor(); }
            else SwitchToThread();
        }
        if (sampler.stop.load()) break;
        if (sequence == consumed) continue;
        skipped += (sequence - consumed) / 2 - 1;
        consumed = sequence;
        const double start = profiler::NowMs();
        PROCESS_MEMORY_COUNTERS_EX counters{};
        counters.cb = sizeof counters;
        const bool have_private = query && query(GetCurrentProcess(),
            reinterpret_cast<PPROCESS_MEMORY_COUNTERS>(&counters), sizeof counters);
        const auto walk = memory::Walk(reinterpret_cast<uintptr_t>(system.lpMinimumApplicationAddress),
            uint64_t(reinterpret_cast<uintptr_t>(system.lpMaximumApplicationAddress)) + 1,
            system.dwAllocationGranularity, QueryRegion, nullptr);
        const double sample_ms = profiler::NowMs() - start;
        writing = WriteSample(sampler.file, buffer, memory::FormatRow(buffer, sizeof buffer,
            frame, t_ms, have_private ? counters.PrivateUsage : 0, walk, sample_ms));
        if (!writing) break;
        {
            std::lock_guard<std::mutex> lock(sampler.summary_mutex);
            ++sampler.samples;
            sampler.skipped = skipped;
            sampler.total_ms += sample_ms;
            if (sample_ms > sampler.max_ms) sampler.max_ms = sample_ms;
        }
    }
    // Only this worker closes the sidecar after ownership is transferred.
    CloseHandle(sampler.file);
    return 0;
}

}  // namespace

bool Start(const std::wstring& path) {
    if (g_sampler.load() || profiler::Stopping()) return false;
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           reinterpret_cast<LPCWSTR>(&Start), &module)) return false;
    auto* sampler = new Sampler;
    sampler->wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    sampler->file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE thread = sampler->wake && sampler->file != INVALID_HANDLE_VALUE ?
        CreateThread(nullptr, 0, Run, sampler, 0, nullptr) : nullptr;
    if (!thread) {
        // No worker exists yet, so ownership has not been transferred.
        if (sampler->file != INVALID_HANDLE_VALUE) CloseHandle(sampler->file);
        if (sampler->wake) CloseHandle(sampler->wake);
        delete sampler;
        return false;
    }
    CloseHandle(thread);
    // Never destroy state or close the wake handle: shutdown may race Request.
    g_sampler.store(sampler);
    if (profiler::Stopping()) RequestStop();
    return true;
}

void Request(uint64_t frame, double t_ms) {
    auto* sampler = g_sampler.load();
    if (!sampler || sampler->stop.load()) return;
    sampler->sequence.fetch_add(1);
    sampler->frame.store(frame);
    sampler->t_ms.store(t_ms);
    sampler->sequence.fetch_add(1);
    SetEvent(sampler->wake);
}

void RequestStop() {
    auto* sampler = g_sampler.load();
    if (!sampler) return;
    sampler->stop.store(true);
    SetEvent(sampler->wake);
}


void WriteSummary(FILE* log) {
    auto* sampler = g_sampler.load();
    if (!sampler || !log) return;
    std::unique_lock<std::mutex> lock(sampler->summary_mutex, std::try_to_lock);
    if (!lock.owns_lock()) return;
    fprintf(log, "[%10.3f] memory sampler: %llu samples, %llu skipped requests, sample_ms mean %.3f max %.3f\n",
        profiler::NowMs(), static_cast<unsigned long long>(sampler->samples),
        static_cast<unsigned long long>(sampler->skipped),
        sampler->samples ? sampler->total_ms / sampler->samples : 0, sampler->max_ms);
}

}  // namespace memory_sampler
