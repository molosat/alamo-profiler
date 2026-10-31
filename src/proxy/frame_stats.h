// Per-frame accumulator and CSV writer. No Windows or D3D dependency so it
// can be unit-tested on any host; the proxy feeds it timestamps in ms.
#pragma once

#include <cstdint>
#include <cstdio>
#include <deque>
#include <string>
#include <vector>

namespace perf {

struct FrameCounters {
    uint32_t draws = 0;
    uint64_t prims = 0;
    uint32_t set_texture = 0;
    uint32_t set_render_state = 0;
    uint32_t set_shader = 0;
    uint32_t set_stream = 0;
    uint32_t set_const = 0;
    uint32_t set_rt = 0;
    uint32_t clears = 0;
    uint32_t scenes = 0;
    double draw_ms = 0.0;  // CPU time spent inside Draw* calls (runtime + driver)
};

struct FrameRow {
    uint64_t frame = 0;
    double t_ms = 0.0;        // end of this frame, relative to the first Present
    double frame_ms = 0.0;    // Present end to Present end
    double present_ms = 0.0;  // time blocked inside Present
    double gpu_ms = -1.0;     // GPU timestamp span; < 0 means not available
    uint32_t marks = 0;
    FrameCounters c;
    std::vector<double> zone_ms;

    // Whatever the frame spent outside Present and Draw*: simulation, AI,
    // scripts, scene traversal, state setting.
    double GameCpuMs() const;
};

class FrameStats {
public:
    // gpu_latency_frames: how long a row waits for its GPU timestamp before
    // it is written without one.
    explicit FrameStats(unsigned gpu_latency_frames = 10);

    // Zones are named extra timing buckets. Register them
    // before the first frame so the CSV header is stable.
    int RegisterZone(const std::string& name);
    void AddZoneTime(int zone, double ms);

    FrameCounters& Current() { return cur_; }
    void AddMark() { ++marks_; }

    // Call when Present returns. The very first call only starts the clock;
    // returns the id of the frame just closed (0 for that first call).
    uint64_t EndFrame(double present_start_ms, double present_end_ms);

    // Attach a GPU time to a frame that is still pending. Unknown ids are ignored.
    void SetGpuTime(uint64_t frame, double gpu_ms);

    void WriteHeader(FILE* out) const;
    // Writes rows that have their GPU time or waited long enough. all = true
    // writes everything (shutdown). A null out discards eligible rows.
    // Returns rows written or discarded.
    size_t Drain(FILE* out, bool all);

    bool Started() const { return started_; }
    double OriginMs() const { return origin_ms_; }
    uint64_t FramesClosed() const { return next_frame_ - 1; }

private:
    void ResetCurrent();

    unsigned gpu_latency_;
    std::vector<std::string> zone_names_;
    std::vector<double> cur_zones_;
    FrameCounters cur_;
    uint32_t marks_ = 0;
    bool started_ = false;
    double origin_ms_ = 0.0;
    double last_end_ms_ = 0.0;
    uint64_t next_frame_ = 1;
    std::deque<FrameRow> pending_;
};

}  // namespace perf
