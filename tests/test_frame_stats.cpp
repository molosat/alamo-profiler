// Host-side unit test for the frame accumulator. Builds with any C++17 compiler:
//   g++ -std=c++17 -I src/proxy tests/test_frame_stats.cpp src/proxy/frame_stats.cpp
#include "frame_stats.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static int g_failures = 0;
#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__,     \
                    __LINE__, #cond);                                  \
            ++g_failures;                                              \
        }                                                              \
    } while (0)

static std::vector<std::string> Lines(FILE* f) {
    std::vector<std::string> out;
    rewind(f);
    char buf[1024];
    while (fgets(buf, sizeof buf, f)) {
        size_t n = strlen(buf);
        if (n && buf[n - 1] == '\n') buf[n - 1] = 0;
        out.push_back(buf);
    }
    return out;
}

static std::vector<std::string> Split(const std::string& s) {
    std::vector<std::string> out;
    size_t start = 0;
    for (;;) {
        size_t comma = s.find(',', start);
        out.push_back(s.substr(start, comma - start));
        if (comma == std::string::npos) return out;
        start = comma + 1;
    }
}

static void TestFirstPresentOnlyStartsClock() {
    perf::FrameStats fs;
    fs.Current().draws = 99;  // load-screen work before the first Present
    CHECK(fs.EndFrame(100.0, 101.0) == 0);
    CHECK(fs.Current().draws == 0);
    FILE* f = tmpfile();
    CHECK(fs.Drain(f, true) == 0);
    fclose(f);
}

static void OriginIsFirstPresentEnd() {
    perf::FrameStats fs;
    CHECK(!fs.Started());
    CHECK(fs.EndFrame(0, 5.0) == 0);
    CHECK(fs.Started());
    CHECK(fs.OriginMs() == 5.0);
    fs.EndFrame(10.0, 15.0);
    CHECK(fs.OriginMs() == 5.0);
}

static void DrainNullDiscards() {
    perf::FrameStats fs;
    fs.EndFrame(0, 5.0);
    for (int i = 1; i <= 50; ++i) fs.EndFrame(i * 10.0, i * 10.0 + 5.0);
    CHECK(fs.FramesClosed() == 50);
    CHECK(fs.Drain(nullptr, true) == 50);
    FILE* f = tmpfile();
    CHECK(f != nullptr);
    if (!f) return;
    CHECK(fs.Drain(f, true) == 0);
    CHECK(Lines(f).empty());
    fclose(f);
}

static void TestRowArithmetic() {
    perf::FrameStats fs;
    int ai = fs.RegisterZone("ai");
    fs.EndFrame(0.0, 10.0);
    fs.Current().draws = 3;
    fs.Current().prims = 300;
    fs.Current().draw_ms = 2.0;
    fs.AddZoneTime(ai, 4.5);
    fs.AddZoneTime(7, 1.0);  // unknown zone is ignored
    fs.AddMark();
    CHECK(fs.EndFrame(25.0, 30.0) == 1);  // frame 20 ms, present 5 ms

    FILE* f = tmpfile();
    fs.WriteHeader(f);
    CHECK(fs.Drain(f, true) == 1);
    std::vector<std::string> lines = Lines(f);
    fclose(f);
    CHECK(lines.size() == 2);
    std::vector<std::string> head = Split(lines[0]);
    std::vector<std::string> row = Split(lines[1]);
    CHECK(head.size() == row.size());
    CHECK(head.back() == "zone_ai");
    CHECK(row[0] == "1");
    CHECK(row[1] == "20.000");   // t_ms relative to first Present end
    CHECK(row[2] == "20.000");   // frame_ms
    CHECK(row[3] == "5.000");    // present_ms
    CHECK(row[4] == "2.000");    // draw_ms
    CHECK(row[5] == "13.000");   // game_cpu_ms = 20 - 5 - 2
    CHECK(row[6] == "");         // no GPU time
    CHECK(row[7] == "3");
    CHECK(row[8] == "300");
    CHECK(row[17] == "1");       // marks
    CHECK(row[18] == "4.500");
}

static void TestGameCpuNeverNegative() {
    perf::FrameRow r;
    r.frame_ms = 5.0;
    r.present_ms = 4.0;
    r.c.draw_ms = 3.0;
    CHECK(r.GameCpuMs() == 0.0);
}

static void TestDrainWaitsForGpuThenGivesUp() {
    perf::FrameStats fs(2);
    fs.EndFrame(0, 0);
    fs.EndFrame(9, 10);  // frame 1
    FILE* f = tmpfile();
    CHECK(fs.Drain(f, false) == 0);  // waiting for GPU result
    fs.SetGpuTime(1, 7.25);
    fs.SetGpuTime(42, 1.0);          // unknown frame ignored
    CHECK(fs.Drain(f, false) == 1);
    fs.EndFrame(19, 20);  // frame 2, never gets a GPU time
    fs.EndFrame(29, 30);  // 3
    CHECK(fs.Drain(f, false) == 0);
    fs.EndFrame(39, 40);  // 4: frame 2 is now 2 frames old
    CHECK(fs.Drain(f, false) == 1);
    CHECK(fs.Drain(f, true) == 2);
    std::vector<std::string> lines = Lines(f);
    fclose(f);
    CHECK(lines.size() == 4);
    CHECK(Split(lines[0])[6] == "7.250");
    CHECK(Split(lines[1])[6] == "");
    CHECK(fs.FramesClosed() == 4);
}

int main() {
    TestFirstPresentOnlyStartsClock();
    OriginIsFirstPresentEnd();
    DrainNullDiscards();
    TestRowArithmetic();
    TestGameCpuNeverNegative();
    TestDrainWaitsForGpuThenGivesUp();
    if (g_failures) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    puts("test_frame_stats: all checks passed");
    return 0;
}
