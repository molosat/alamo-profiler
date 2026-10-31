#include "memory_sample.h"

#include <algorithm>
#include <cstdio>
#include <limits>

namespace memory {

WalkResult Walk(uint64_t minimum, uint64_t end, uint64_t granularity,
                Query query, void* context, unsigned cap) {
    WalkResult result;
    uint64_t address = minimum;
    if (!granularity || !query) { result.truncated = true; return result; }
    while (address < end) {
        if (result.regions == cap) { result.truncated = true; break; }
        Region region;
        ++result.regions;
        if (!query(address, &region, context) || !region.size || region.base > address ||
            region.size > std::numeric_limits<uint64_t>::max() - region.base) {
            result.truncated = true;
            break;
        }
        const uint64_t next = region.base + region.size;
        if (next <= address) { result.truncated = true; break; }
        if (region.is_free) {
            const uint64_t base = std::max(address, region.base);
            const uint64_t limit = std::min(next, end);
            const uint64_t remainder = base % granularity;
            const uint64_t padding = remainder ? granularity - remainder : 0;
            const uint64_t usable = limit - base;
            if (padding < usable)
                result.largest_free_bytes = std::max(result.largest_free_bytes, usable - padding);
        }
        address = next;
    }
    return result;
}

size_t FormatHeader(char* buffer, size_t capacity) {
    if (!buffer || !capacity) return 0;
    const int length = snprintf(buffer, capacity,
        "frame,t_ms,private_bytes,largest_free_bytes,regions,truncated,sample_ms\n");
    return length >= 0 && static_cast<size_t>(length) < capacity ? length : 0;
}

size_t FormatRow(char* buffer, size_t capacity, uint64_t frame, double t_ms,
                 uint64_t private_bytes, const WalkResult& walk, double sample_ms) {
    if (!buffer || !capacity) return 0;
    const int length = snprintf(buffer, capacity, "%llu,%.3f,%llu,%llu,%u,%d,%.3f\n",
            static_cast<unsigned long long>(frame), t_ms,
            static_cast<unsigned long long>(private_bytes),
            static_cast<unsigned long long>(walk.largest_free_bytes), walk.regions,
            walk.truncated ? 1 : 0, sample_ms);
    return length >= 0 && static_cast<size_t>(length) < capacity ? length : 0;
}

}  // namespace memory
