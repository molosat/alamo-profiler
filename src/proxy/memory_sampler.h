// Process-lifetime background sampler. No waits or allocations in Request.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

namespace memory_sampler {

bool Start(const std::wstring& path);
// Serialized by profiler's frame lock; keeps only the newest pending request.
void Request(uint64_t frame, double t_ms);
void RequestStop();
// Best-effort snapshot of completed samples; never waits for the worker.
void WriteSummary(FILE* log);

}  // namespace memory_sampler
