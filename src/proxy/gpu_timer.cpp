#include "gpu_timer.h"

#include "profiler.h"

namespace gpu {
namespace {

enum class SlotState { kFree, kOpen, kIssued };

struct Slot {
    IDirect3DQuery9* disjoint = nullptr;
    IDirect3DQuery9* freq = nullptr;
    IDirect3DQuery9* begin = nullptr;
    IDirect3DQuery9* end = nullptr;
    uint64_t frame = 0;
    SlotState state = SlotState::kFree;
};

// Drivers queue up to three frames ahead by default; the extra slots cover
// very short frames (menus, loading) where more are in flight.
constexpr int kSlots = 8;

IDirect3DDevice9* g_dev = nullptr;
Slot g_slots[kSlots];
int g_open = -1;
int g_last_issued = -1;
unsigned long g_held_refs = 0;

unsigned long DeviceRefs(IDirect3DDevice9* dev) {
    IDirect3DDevice9_AddRef(dev);
    return IDirect3DDevice9_Release(dev);
}

void ReleaseQuery(IDirect3DQuery9*& q) {
    if (q) IDirect3DQuery9_Release(q);
    q = nullptr;
}

template <class T>
HRESULT Get(IDirect3DQuery9* q, T* out) {
    return IDirect3DQuery9_GetData(q, out, sizeof(T), 0);
}

}  // namespace

void Detach() {
    for (Slot& s : g_slots) {
        ReleaseQuery(s.disjoint);
        ReleaseQuery(s.freq);
        ReleaseQuery(s.begin);
        ReleaseQuery(s.end);
        s.state = SlotState::kFree;
    }
    g_dev = nullptr;
    g_open = -1;
    g_last_issued = -1;
    g_held_refs = 0;
}

void Attach(IDirect3DDevice9* dev) {
    Detach();
    if (!profiler::Cfg().gpu_timing) return;
    // Check support first: passing NULL only asks whether the type exists.
    if (FAILED(IDirect3DDevice9_CreateQuery(dev, D3DQUERYTYPE_TIMESTAMP, nullptr)) ||
        FAILED(IDirect3DDevice9_CreateQuery(dev, D3DQUERYTYPE_TIMESTAMPDISJOINT, nullptr)) ||
        FAILED(IDirect3DDevice9_CreateQuery(dev, D3DQUERYTYPE_TIMESTAMPFREQ, nullptr))) {
        profiler::Log("gpu timing: timestamp queries unsupported, gpu_ms will be empty");
        return;
    }
    unsigned long before = DeviceRefs(dev);
    for (Slot& s : g_slots) {
        if (FAILED(IDirect3DDevice9_CreateQuery(dev, D3DQUERYTYPE_TIMESTAMPDISJOINT, &s.disjoint)) ||
            FAILED(IDirect3DDevice9_CreateQuery(dev, D3DQUERYTYPE_TIMESTAMPFREQ, &s.freq)) ||
            FAILED(IDirect3DDevice9_CreateQuery(dev, D3DQUERYTYPE_TIMESTAMP, &s.begin)) ||
            FAILED(IDirect3DDevice9_CreateQuery(dev, D3DQUERYTYPE_TIMESTAMP, &s.end))) {
            profiler::Log("gpu timing: CreateQuery failed, gpu_ms will be empty");
            Detach();
            return;
        }
    }
    g_dev = dev;
    g_held_refs = DeviceRefs(dev) - before;
    profiler::Log("gpu timing: %d query slots, holding %lu device refs", kSlots, g_held_refs);
}

IDirect3DDevice9* Device() { return g_dev; }
unsigned long HeldDeviceRefs() { return g_held_refs; }

void BeginFrame() {
    if (!g_dev || g_open >= 0) return;
    for (int i = 0; i < kSlots; ++i) {
        Slot& s = g_slots[i];
        if (s.state != SlotState::kFree) continue;
        IDirect3DQuery9_Issue(s.disjoint, D3DISSUE_BEGIN);
        IDirect3DQuery9_Issue(s.begin, D3DISSUE_END);
        s.state = SlotState::kOpen;
        s.frame = 0;
        g_open = i;
        return;
    }
    // Every slot still in flight: this frame goes without a GPU time.
}

void EndFrame() {
    if (g_open < 0) return;
    Slot& s = g_slots[g_open];
    IDirect3DQuery9_Issue(s.end, D3DISSUE_END);
    IDirect3DQuery9_Issue(s.freq, D3DISSUE_END);
    IDirect3DQuery9_Issue(s.disjoint, D3DISSUE_END);
    s.state = SlotState::kIssued;
    g_last_issued = g_open;
    g_open = -1;
}

void TagLastFrame(uint64_t frame) {
    if (g_last_issued >= 0) g_slots[g_last_issued].frame = frame;
    g_last_issued = -1;
}

void Poll() {
    for (Slot& s : g_slots) {
        if (s.state != SlotState::kIssued) continue;
        BOOL disjoint = FALSE;
        UINT64 freq = 0, t0 = 0, t1 = 0;
        HRESULT hr = Get(s.disjoint, &disjoint);
        if (hr == S_OK) hr = Get(s.freq, &freq);
        if (hr == S_OK) hr = Get(s.begin, &t0);
        if (hr == S_OK) hr = Get(s.end, &t1);
        if (hr == S_FALSE) continue;  // not ready yet; try next frame
        // S_OK, or an error such as device lost: either way the slot is done.
        if (hr == S_OK && !disjoint && freq > 0 && t1 >= t0 && s.frame > 0)
            profiler::Stats().SetGpuTime(s.frame, static_cast<double>(t1 - t0) * 1000.0 /
                                                   static_cast<double>(freq));
        s.state = SlotState::kFree;
    }
}

}  // namespace gpu
