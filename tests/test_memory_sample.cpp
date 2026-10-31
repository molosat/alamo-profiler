#include "memory_sample.h"

#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
    ++g_failures; } } while (0)

static bool Query(uint64_t address, memory::Region* out, void* context) {
    for (const auto& region : *static_cast<std::vector<memory::Region>*>(context)) {
        if (address >= region.base && address - region.base < region.size) {
            *out = region;
            return true;
        }
    }
    return false;
}

static bool Constant(uint64_t, memory::Region* out, void* context) {
    *out = *static_cast<memory::Region*>(context);
    return true;
}

int main() {
    std::vector<memory::Region> regions{{0, 64, false}, {64, 64, true},
        {128, 32, false}, {160, 160, true}, {320, 64, true}};
    auto walk = memory::Walk(0, 384, 1, Query, &regions);
    CHECK(walk.largest_free_bytes == 160 && walk.regions == 5 && !walk.truncated);
    walk = memory::Walk(0, 384, 64, Query, &regions);
    CHECK(walk.largest_free_bytes == 128 && !walk.truncated);
    // A block ending before the next aligned base is unusable.
    memory::Region small{1, 31, true};
    CHECK(memory::Walk(1, 32, 64, Constant, &small).largest_free_bytes == 0);
    walk = memory::Walk(0, 384, 1, Query, &regions, 3);
    CHECK(walk.regions == 3 && walk.truncated && walk.largest_free_bytes == 64);
    CHECK(memory::Walk(0, 384, 1, Query, &regions, 0).truncated);
    CHECK(!memory::Walk(0, 128, 1, Query, &regions, 2).truncated);

    memory::Region stuck{0, 64, true};
    walk = memory::Walk(0, 128, 1, Constant, &stuck);
    CHECK(walk.regions == 2 && walk.truncated);
    stuck.size = 0;
    CHECK(memory::Walk(0, 128, 1, Constant, &stuck).truncated);
    stuck = {std::numeric_limits<uint64_t>::max() - 1, 4, true};
    CHECK(memory::Walk(stuck.base, stuck.base + 1, 1, Constant, &stuck).truncated);
    walk = memory::Walk(0, 400, 1, Query, &regions);
    CHECK(walk.truncated && walk.regions == 6);
    CHECK(memory::Walk(0, 384, 0, Query, &regions).truncated);
    // Clip both ends of a region to the requested range; keep 64-bit sizes.
    memory::Region huge{0, uint64_t(1) << 44, true};
    walk = memory::Walk(1, huge.size - 1, 65536, Constant, &huge);
    CHECK(walk.largest_free_bytes == huge.size - 1 - 65536 && !walk.truncated);

    char text[1024]{};
    const char* header = "frame,t_ms,private_bytes,largest_free_bytes,regions,truncated,sample_ms\n";
    CHECK(memory::FormatHeader(text, sizeof text) == strlen(header));
    CHECK(strcmp(text, header) == 0);
    const char* row = "60,1234.568,8589934592,17592186044416,42,1,2.346\n";
    CHECK(memory::FormatRow(text, sizeof text, 60, 1234.5678, uint64_t(1) << 33,
                            {uint64_t(1) << 44, 42, true}, 2.3456) == strlen(row));
    CHECK(strcmp(text, row) == 0);
    CHECK(memory::FormatRow(text, sizeof text, 1, 0, 0, {}, 0) == 22);
    CHECK(strcmp(text, "1,0.000,0,0,0,0,0.000\n") == 0);
    char tiny[4] = {'x', 'x', 'x', 'x'};
    CHECK(memory::FormatHeader(tiny, sizeof tiny) == 0);
    CHECK(tiny[sizeof tiny - 1] == '\0');
    tiny[sizeof tiny - 1] = 'x';
    CHECK(memory::FormatRow(tiny, sizeof tiny, 60, 1234, 0, {}, 0) == 0);
    CHECK(tiny[sizeof tiny - 1] == '\0');
    CHECK(memory::FormatHeader(nullptr, 0) == 0);
    CHECK(memory::FormatRow(text, 0, 0, 0, 0, {}, 0) == 0);
    printf("%s: memory sample\n", g_failures ? "FAIL" : "PASS");
    return g_failures ? 1 : 0;
}
