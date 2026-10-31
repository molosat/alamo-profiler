// Address-space walk and CSV formatting, independent of Windows.
#pragma once

#include <cstdint>
#include <cstddef>

namespace memory {

struct Region {
    uint64_t base = 0;
    uint64_t size = 0;
    bool is_free = false;
};

struct WalkResult {
    uint64_t largest_free_bytes = 0;
    unsigned regions = 0;
    bool truncated = false;
};

using Query = bool (*)(uint64_t address, Region* region, void* context);
// Walk [minimum, end). The cap counts query calls, including a failed call.
WalkResult Walk(uint64_t minimum, uint64_t end, uint64_t granularity,
                Query query, void* context, unsigned cap = 1u << 20);

// Return the length excluding the NUL, or 0 on error/insufficient capacity.
// A nonempty buffer is NUL-terminated even when the text does not fit.
size_t FormatHeader(char* buffer, size_t capacity);
size_t FormatRow(char* buffer, size_t capacity, uint64_t frame, double t_ms,
                 uint64_t private_bytes, const WalkResult& walk, double sample_ms);

}  // namespace memory
