#include "frame_stats.h"

namespace perf {

double FrameRow::GameCpuMs() const {
    double v = frame_ms - present_ms - c.draw_ms;
    return v > 0.0 ? v : 0.0;
}

FrameStats::FrameStats(unsigned gpu_latency_frames) : gpu_latency_(gpu_latency_frames) {}

int FrameStats::RegisterZone(const std::string& name) {
    zone_names_.push_back(name);
    cur_zones_.push_back(0.0);
    return static_cast<int>(zone_names_.size() - 1);
}

void FrameStats::AddZoneTime(int zone, double ms) {
    if (zone >= 0 && static_cast<size_t>(zone) < cur_zones_.size()) cur_zones_[zone] += ms;
}

void FrameStats::ResetCurrent() {
    cur_ = FrameCounters();
    marks_ = 0;
    for (double& z : cur_zones_) z = 0.0;
}

uint64_t FrameStats::EndFrame(double present_start_ms, double present_end_ms) {
    if (!started_) {
        // Everything before the first Present is load time, not a frame.
        started_ = true;
        origin_ms_ = present_end_ms;
        last_end_ms_ = present_end_ms;
        ResetCurrent();
        return 0;
    }
    FrameRow row;
    row.frame = next_frame_++;
    row.t_ms = present_end_ms - origin_ms_;
    row.frame_ms = present_end_ms - last_end_ms_;
    row.present_ms = present_end_ms - present_start_ms;
    row.marks = marks_;
    row.c = cur_;
    row.zone_ms = cur_zones_;
    pending_.push_back(row);
    last_end_ms_ = present_end_ms;
    ResetCurrent();
    return row.frame;
}

void FrameStats::SetGpuTime(uint64_t frame, double gpu_ms) {
    for (FrameRow& r : pending_) {
        if (r.frame == frame) {
            r.gpu_ms = gpu_ms;
            return;
        }
    }
}

void FrameStats::WriteHeader(FILE* out) const {
    fputs("frame,t_ms,frame_ms,present_ms,draw_ms,game_cpu_ms,gpu_ms,"
          "draws,prims,set_texture,set_render_state,set_shader,set_stream,"
          "set_const,set_rt,clears,scenes,marks", out);
    for (const std::string& z : zone_names_) fprintf(out, ",zone_%s", z.c_str());
    fputc('\n', out);
}

size_t FrameStats::Drain(FILE* out, bool all) {
    size_t written = 0;
    uint64_t newest = next_frame_ - 1;
    while (!pending_.empty()) {
        const FrameRow& r = pending_.front();
        bool ready = all || r.gpu_ms >= 0.0 || newest - r.frame >= gpu_latency_;
        if (!ready) break;
        if (out) {
            fprintf(out, "%llu,%.3f,%.3f,%.3f,%.3f,%.3f,",
                    static_cast<unsigned long long>(r.frame), r.t_ms, r.frame_ms,
                    r.present_ms, r.c.draw_ms, r.GameCpuMs());
            if (r.gpu_ms >= 0.0) fprintf(out, "%.3f", r.gpu_ms);
            fprintf(out, ",%u,%llu,%u,%u,%u,%u,%u,%u,%u,%u,%u", r.c.draws,
                    static_cast<unsigned long long>(r.c.prims), r.c.set_texture,
                    r.c.set_render_state, r.c.set_shader, r.c.set_stream, r.c.set_const,
                    r.c.set_rt, r.c.clears, r.c.scenes, r.marks);
            for (double z : r.zone_ms) fprintf(out, ",%.3f", z);
            fputc('\n', out);
        }
        pending_.pop_front();
        ++written;
    }
    return written;
}

}  // namespace perf
