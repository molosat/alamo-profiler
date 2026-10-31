#include "device_hooks.h"

#include "gpu_timer.h"
#include "profiler.h"

namespace hooks {
namespace {

// Originals, captured from the first object of each kind. A process sees one
// D3D9 runtime, so every device's slots start out holding these same
// functions; a slot holding anything else is logged and left alone rather
// than risk calling the wrong original.
//
// On Windows 10/11 the runtime gives each device its own vtable copy on the
// heap and rewrites it in place: BeginStateBlock swaps in recording versions
// of the Set* methods, and EndStateBlock copies the normal table back over
// our hooks. D3DX effects record state blocks while the game loads, so the
// hooks are re-applied after every EndStateBlock and Reset.
struct D3D9Orig {
    decltype(IDirect3D9Vtbl::CreateDevice) CreateDevice;
};
struct D3D9ExOrig {
    decltype(IDirect3D9ExVtbl::CreateDevice) CreateDevice;
    decltype(IDirect3D9ExVtbl::CreateDeviceEx) CreateDeviceEx;
};
struct DeviceOrig {
    decltype(IDirect3DDevice9Vtbl::Release) Release;
    decltype(IDirect3DDevice9Vtbl::Reset) Reset;
    decltype(IDirect3DDevice9Vtbl::Present) Present;
    decltype(IDirect3DDevice9Vtbl::BeginScene) BeginScene;
    decltype(IDirect3DDevice9Vtbl::Clear) Clear;
    decltype(IDirect3DDevice9Vtbl::DrawPrimitive) DrawPrimitive;
    decltype(IDirect3DDevice9Vtbl::DrawIndexedPrimitive) DrawIndexedPrimitive;
    decltype(IDirect3DDevice9Vtbl::DrawPrimitiveUP) DrawPrimitiveUP;
    decltype(IDirect3DDevice9Vtbl::DrawIndexedPrimitiveUP) DrawIndexedPrimitiveUP;
    decltype(IDirect3DDevice9Vtbl::SetTexture) SetTexture;
    decltype(IDirect3DDevice9Vtbl::SetRenderState) SetRenderState;
    decltype(IDirect3DDevice9Vtbl::SetVertexShader) SetVertexShader;
    decltype(IDirect3DDevice9Vtbl::SetPixelShader) SetPixelShader;
    decltype(IDirect3DDevice9Vtbl::SetStreamSource) SetStreamSource;
    decltype(IDirect3DDevice9Vtbl::SetVertexShaderConstantF) SetVertexShaderConstantF;
    decltype(IDirect3DDevice9Vtbl::SetPixelShaderConstantF) SetPixelShaderConstantF;
    decltype(IDirect3DDevice9Vtbl::SetRenderTarget) SetRenderTarget;
    decltype(IDirect3DDevice9Vtbl::BeginStateBlock) BeginStateBlock;
    decltype(IDirect3DDevice9Vtbl::EndStateBlock) EndStateBlock;
};
struct SwapChainOrig {
    decltype(IDirect3DSwapChain9Vtbl::Present) Present;
};

D3D9Orig g_d3d;
D3D9ExOrig g_d3dex;
DeviceOrig g_dev;
SwapChainOrig g_swap;
// EndStateBlock as the recording table has it, between BeginStateBlock and
// EndStateBlock; null when not recording.
decltype(IDirect3DDevice9Vtbl::EndStateBlock) g_rec_end = nullptr;
unsigned long g_state_blocks = 0;
DWORD g_render_thread = 0;
bool g_thread_warned = false;
bool g_in_release = false;
bool g_frame_open = false;

template <class F>
void WriteSlot(F* slot, F value) {
    DWORD old;
    if (!VirtualProtect(slot, sizeof *slot, PAGE_EXECUTE_READWRITE, &old)) {
        profiler::Log("ERROR: VirtualProtect failed (%lu)", GetLastError());
        return;
    }
    *slot = value;
    VirtualProtect(slot, sizeof *slot, old, &old);
}

// Hooks a slot that holds the original. The first call records the original;
// later calls (other devices, re-application after EndStateBlock) only patch
// a slot that still holds that same function.
template <class F>
void Patch(F* slot, F hook, F* orig, const char* name) {
    if (*slot == hook) return;
    if (!*orig) {
        *orig = *slot;
    } else if (*slot != *orig) {
        static int warnings = 0;
        if (++warnings <= 10)
            profiler::Log("WARNING: %s slot holds an unknown function; left unhooked", name);
        return;
    }
    WriteSlot(slot, hook);
}

#define PATCH(vt, orig, name) \
    Patch(const_cast<decltype(orig.name)*>(&(vt)->name), hk##name, &orig.name, #name)

void CheckThread(const char* where) {
    if (g_thread_warned || GetCurrentThreadId() == g_render_thread) return;
    g_thread_warned = true;
    profiler::Log("WARNING: %s called from thread %lu, device created on %lu; "
               "counters assume a single render thread", where, GetCurrentThreadId(),
               g_render_thread);
}

// Nested hooks contribute their own pre-runtime work. Remove their full wall
// time from a still-running parent, including any nested real Present wait.
class SelfTime {
public:
    SelfTime() : enabled_(profiler::Cfg().frames_csv) {
        if (!enabled_) return;
        start_ = profiler::NowMs();
        parent_ = current_;
        current_ = this;
    }
    ~SelfTime() {
        if (!enabled_) return;
        Finish();
        current_ = parent_;
        if (parent_ && !parent_->finished_)
            parent_->children_ms_ += profiler::NowMs() - start_;
    }
    void Finish() {
        if (!enabled_ || finished_) return;
        profiler::AddSelfTime(profiler::NowMs() - start_ - children_ms_);
        finished_ = true;
    }
private:
    static thread_local SelfTime* current_;
    SelfTime* parent_ = nullptr;
    double start_ = 0.0, children_ms_ = 0.0;
    bool enabled_, finished_ = false;
};
thread_local SelfTime* SelfTime::current_ = nullptr;

void BeginFrameIfNeeded() {
    if (g_frame_open) return;
    g_frame_open = true;
    SelfTime self_time;
    gpu::BeginFrame();
}

void FrameEnd(double t0) {
    double t1 = profiler::NowMs();
    g_frame_open = false;
    profiler::OnFrameEnd(t0, t1);
}

// --- device -----------------------------------------------------------------

void PatchDevice(IDirect3DDevice9Vtbl* v);

ULONG STDMETHODCALLTYPE hkRelease(IDirect3DDevice9* d) {
    ULONG n = g_dev.Release(d);
    // Our timestamp queries hold device references. When they are all that
    // is left, the game has let go of the device: free them so it is
    // destroyed now and the game sees the 0 it expects.
    if (!g_in_release && d == gpu::Device() && gpu::HeldDeviceRefs() > 0 &&
        n == gpu::HeldDeviceRefs()) {
        g_in_release = true;
        gpu::Detach();
        g_in_release = false;
        profiler::Log("device released by the game");
        profiler::FlushCsv();
        return 0;
    }
    // Without timestamp queries (disabled or unsupported), the runtime's
    // Release reaches zero directly. Drain before a later device adds rows.
    if (n == 0) { profiler::FlushCsv(); }
    return n;
}

// A failed Reset leaves the queries detached; the retry that succeeds restores them.
IDirect3DDevice9* g_gpu_awaiting_reset = nullptr;

HRESULT STDMETHODCALLTYPE hkReset(IDirect3DDevice9* d, D3DPRESENT_PARAMETERS* pp) {
    bool had_gpu = gpu::Device() == d || g_gpu_awaiting_reset == d;
    g_in_release = true;
    if (had_gpu) gpu::Detach();
    g_in_release = false;
    HRESULT hr = g_dev.Reset(d, pp);
    PatchDevice(d->lpVtbl);  // in case the runtime rebuilt the vtable
    profiler::Log("Reset -> 0x%08lX%s", static_cast<unsigned long>(hr),
               pp ? "" : " (null params)");
    if (pp)
        profiler::Log("  backbuffer %ux%u windowed=%d interval=0x%X", pp->BackBufferWidth,
                   pp->BackBufferHeight, pp->Windowed, pp->PresentationInterval);
    g_in_release = true;
    if (had_gpu && SUCCEEDED(hr)) gpu::Attach(d);
    g_gpu_awaiting_reset = had_gpu && FAILED(hr) ? d : nullptr;
    g_in_release = false;
    g_frame_open = false;
    return hr;
}

// BeginStateBlock replaces the whole table with its recording version, which
// drops every hook. Only EndStateBlock is hooked again, through the recording
// table's own function, so the recorded Set* calls stay untouched.
HRESULT STDMETHODCALLTYPE hkEndStateBlock(IDirect3DDevice9* d, IDirect3DStateBlock9** sb);

HRESULT STDMETHODCALLTYPE hkBeginStateBlock(IDirect3DDevice9* d) {
    HRESULT hr = g_dev.BeginStateBlock(d);
    IDirect3DDevice9Vtbl* v = d->lpVtbl;
    if (SUCCEEDED(hr) && v->EndStateBlock != hkEndStateBlock) {
        g_rec_end = v->EndStateBlock;
        WriteSlot(const_cast<decltype(g_rec_end)*>(&v->EndStateBlock), &hkEndStateBlock);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hkEndStateBlock(IDirect3DDevice9* d, IDirect3DStateBlock9** sb) {
    auto end = g_rec_end ? g_rec_end : g_dev.EndStateBlock;
    g_rec_end = nullptr;
    HRESULT hr = end(d, sb);
    PatchDevice(d->lpVtbl);
    if (++g_state_blocks == 1) profiler::Log("state block recorded; hooks re-applied");
    return hr;
}

HRESULT STDMETHODCALLTYPE hkPresent(IDirect3DDevice9* d, const RECT* src, const RECT* dst,
                                    HWND wnd, const RGNDATA* dirty) {
    SelfTime self_time;
    if (profiler::InsidePresent()) {
        self_time.Finish();
        return g_dev.Present(d,src,dst,wnd,dirty);
    }
    profiler::EnterPresent();
    struct PresentScope { ~PresentScope() { profiler::LeavePresent(); } } present_scope;
    CheckThread("Present");
    double t0 = profiler::NowMs();
    gpu::EndFrame();
    self_time.Finish();
    HRESULT hr = g_dev.Present(d, src, dst, wnd, dirty);
    FrameEnd(t0);
    return hr;
}

HRESULT STDMETHODCALLTYPE hkBeginScene(IDirect3DDevice9* d) {
    ++profiler::Stats().Current().scenes;
    BeginFrameIfNeeded();
    return g_dev.BeginScene(d);
}

HRESULT STDMETHODCALLTYPE hkClear(IDirect3DDevice9* d, DWORD count, const D3DRECT* rects,
                                  DWORD flags, D3DCOLOR color, float z, DWORD stencil) {
    ++profiler::Stats().Current().clears;
    BeginFrameIfNeeded();
    return g_dev.Clear(d, count, rects, flags, color, z, stencil);
}

// Each draw counts itself and, with DrawTiming on, adds the CPU time spent in
// the runtime and driver for that call.
#define TIMED_DRAW(prim_count, call)                       \
    perf::FrameCounters& c = profiler::Stats().Current();     \
    ++c.draws;                                             \
    c.prims += (prim_count);                               \
    if (!profiler::Cfg().draw_timing) return call;            \
    double t0 = profiler::NowMs();                            \
    HRESULT hr = call;                                     \
    c.draw_ms += profiler::NowMs() - t0;                      \
    return hr

HRESULT STDMETHODCALLTYPE hkDrawPrimitive(IDirect3DDevice9* d, D3DPRIMITIVETYPE type,
                                          UINT start, UINT prims) {
    TIMED_DRAW(prims, g_dev.DrawPrimitive(d, type, start, prims));
}

HRESULT STDMETHODCALLTYPE hkDrawIndexedPrimitive(IDirect3DDevice9* d, D3DPRIMITIVETYPE type,
                                                 INT base, UINT min_index, UINT verts,
                                                 UINT start, UINT prims) {
    CheckThread("DrawIndexedPrimitive");
    TIMED_DRAW(prims, g_dev.DrawIndexedPrimitive(d, type, base, min_index, verts, start, prims));
}

HRESULT STDMETHODCALLTYPE hkDrawPrimitiveUP(IDirect3DDevice9* d, D3DPRIMITIVETYPE type,
                                            UINT prims, const void* data, UINT stride) {
    TIMED_DRAW(prims, g_dev.DrawPrimitiveUP(d, type, prims, data, stride));
}

HRESULT STDMETHODCALLTYPE hkDrawIndexedPrimitiveUP(IDirect3DDevice9* d, D3DPRIMITIVETYPE type,
                                                   UINT min_index, UINT verts, UINT prims,
                                                   const void* indices, D3DFORMAT fmt,
                                                   const void* data, UINT stride) {
    TIMED_DRAW(prims, g_dev.DrawIndexedPrimitiveUP(d, type, min_index, verts, prims, indices,
                                                   fmt, data, stride));
}

HRESULT STDMETHODCALLTYPE hkSetTexture(IDirect3DDevice9* d, DWORD stage,
                                       IDirect3DBaseTexture9* tex) {
    ++profiler::Stats().Current().set_texture;
    return g_dev.SetTexture(d, stage, tex);
}

HRESULT STDMETHODCALLTYPE hkSetRenderState(IDirect3DDevice9* d, D3DRENDERSTATETYPE state,
                                           DWORD value) {
    ++profiler::Stats().Current().set_render_state;
    return g_dev.SetRenderState(d, state, value);
}

HRESULT STDMETHODCALLTYPE hkSetVertexShader(IDirect3DDevice9* d, IDirect3DVertexShader9* s) {
    ++profiler::Stats().Current().set_shader;
    return g_dev.SetVertexShader(d, s);
}

HRESULT STDMETHODCALLTYPE hkSetPixelShader(IDirect3DDevice9* d, IDirect3DPixelShader9* s) {
    ++profiler::Stats().Current().set_shader;
    return g_dev.SetPixelShader(d, s);
}

HRESULT STDMETHODCALLTYPE hkSetStreamSource(IDirect3DDevice9* d, UINT stream,
                                            IDirect3DVertexBuffer9* vb, UINT offset,
                                            UINT stride) {
    ++profiler::Stats().Current().set_stream;
    return g_dev.SetStreamSource(d, stream, vb, offset, stride);
}

HRESULT STDMETHODCALLTYPE hkSetVertexShaderConstantF(IDirect3DDevice9* d, UINT reg,
                                                     const float* data, UINT count) {
    ++profiler::Stats().Current().set_const;
    return g_dev.SetVertexShaderConstantF(d, reg, data, count);
}

HRESULT STDMETHODCALLTYPE hkSetPixelShaderConstantF(IDirect3DDevice9* d, UINT reg,
                                                    const float* data, UINT count) {
    ++profiler::Stats().Current().set_const;
    return g_dev.SetPixelShaderConstantF(d, reg, data, count);
}

HRESULT STDMETHODCALLTYPE hkSetRenderTarget(IDirect3DDevice9* d, DWORD index,
                                            IDirect3DSurface9* rt) {
    ++profiler::Stats().Current().set_rt;
    return g_dev.SetRenderTarget(d, index, rt);
}

// --- swap chain -------------------------------------------------------------

HRESULT STDMETHODCALLTYPE hkSwapPresent(IDirect3DSwapChain9* sc, const RECT* src,
                                        const RECT* dst, HWND wnd, const RGNDATA* dirty,
                                        DWORD flags) {
    SelfTime self_time;
    if (profiler::InsidePresent()) {
        self_time.Finish();
        return g_swap.Present(sc,src,dst,wnd,dirty,flags);
    }
    profiler::EnterPresent();
    struct PresentScope { ~PresentScope() { profiler::LeavePresent(); } } present_scope;
    CheckThread("IDirect3DSwapChain9::Present");
    double t0 = profiler::NowMs();
    gpu::EndFrame();
    self_time.Finish();
    HRESULT hr = g_swap.Present(sc, src, dst, wnd, dirty, flags);
    FrameEnd(t0);
    return hr;
}

// --- device creation --------------------------------------------------------

const char* DevType(D3DDEVTYPE t) {
    switch (t) {
        case D3DDEVTYPE_HAL: return "HAL";
        case D3DDEVTYPE_REF: return "REF";
        case D3DDEVTYPE_SW: return "SW";
        case D3DDEVTYPE_NULLREF: return "NULLREF";
        default: return "?";
    }
}

void LogDevice(IDirect3D9* d3d, UINT adapter, D3DDEVTYPE type, DWORD flags,
               const D3DPRESENT_PARAMETERS* pp) {
    D3DADAPTER_IDENTIFIER9 id = {};
    if (SUCCEEDED(IDirect3D9_GetAdapterIdentifier(d3d, adapter, 0, &id))) {
        LARGE_INTEGER v = id.DriverVersion;
        profiler::Log("adapter %u: %s (%s %u.%u.%u.%u)", adapter, id.Description, id.Driver,
                   HIWORD(v.HighPart), LOWORD(v.HighPart), HIWORD(v.LowPart),
                   LOWORD(v.LowPart));
    }
    profiler::Log("device type %s, behavior flags 0x%lX:%s%s%s%s%s", DevType(type),
               static_cast<unsigned long>(flags),
               (flags & D3DCREATE_HARDWARE_VERTEXPROCESSING) ? " HW_VP" : "",
               (flags & D3DCREATE_SOFTWARE_VERTEXPROCESSING) ? " SW_VP" : "",
               (flags & D3DCREATE_MIXED_VERTEXPROCESSING) ? " MIXED_VP" : "",
               (flags & D3DCREATE_PUREDEVICE) ? " PURE" : "",
               (flags & D3DCREATE_MULTITHREADED) ? " MULTITHREADED" : "");
    if (!pp) return;
    const char* vsync = pp->PresentationInterval == D3DPRESENT_INTERVAL_IMMEDIATE
                            ? "off (IMMEDIATE)"
                            : "on - present_ms includes vsync waits";
    profiler::Log("backbuffer %ux%u fmt=%d count=%u msaa=%d windowed=%d refresh=%u",
               pp->BackBufferWidth, pp->BackBufferHeight, pp->BackBufferFormat,
               pp->BackBufferCount, pp->MultiSampleType, pp->Windowed,
               pp->FullScreen_RefreshRateInHz);
    profiler::Log("presentation interval 0x%X: vsync %s", pp->PresentationInterval, vsync);
}

void PatchDevice(IDirect3DDevice9Vtbl* v) {
    PATCH(v, g_dev, Release);
    PATCH(v, g_dev, Reset);
    PATCH(v, g_dev, Present);
    PATCH(v, g_dev, BeginScene);
    PATCH(v, g_dev, Clear);
    PATCH(v, g_dev, DrawPrimitive);
    PATCH(v, g_dev, DrawIndexedPrimitive);
    PATCH(v, g_dev, DrawPrimitiveUP);
    PATCH(v, g_dev, DrawIndexedPrimitiveUP);
    PATCH(v, g_dev, SetTexture);
    PATCH(v, g_dev, SetRenderState);
    PATCH(v, g_dev, SetVertexShader);
    PATCH(v, g_dev, SetPixelShader);
    PATCH(v, g_dev, SetStreamSource);
    PATCH(v, g_dev, SetVertexShaderConstantF);
    PATCH(v, g_dev, SetPixelShaderConstantF);
    PATCH(v, g_dev, SetRenderTarget);
    PATCH(v, g_dev, BeginStateBlock);
    PATCH(v, g_dev, EndStateBlock);
}

void OnDeviceCreated(IDirect3D9* d3d, UINT adapter, D3DDEVTYPE type, DWORD flags,
                     const D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9* dev) {
    LogDevice(d3d, adapter, type, flags, pp);
    g_render_thread = GetCurrentThreadId();
    PatchDevice(dev->lpVtbl);

    IDirect3DSwapChain9* sc = nullptr;
    if (SUCCEEDED(IDirect3DDevice9_GetSwapChain(dev, 0, &sc)) && sc) {
        Patch(&sc->lpVtbl->Present, hkSwapPresent, &g_swap.Present, "SwapChain::Present");
        IDirect3DSwapChain9_Release(sc);
    }

    profiler::OpenFramesCsv();
    g_in_release = true;
    gpu::Attach(dev);
    g_in_release = false;
    g_frame_open = false;
    profiler::Log("device hooked");
}

HRESULT STDMETHODCALLTYPE hkCreateDevice(IDirect3D9* d3d, UINT adapter, D3DDEVTYPE type,
                                         HWND wnd, DWORD flags, D3DPRESENT_PARAMETERS* pp,
                                         IDirect3DDevice9** out) {
    HRESULT hr = g_d3d.CreateDevice(d3d, adapter, type, wnd, flags, pp, out);
    profiler::Log("CreateDevice -> 0x%08lX", static_cast<unsigned long>(hr));
    if (SUCCEEDED(hr) && out && *out) {
        OnDeviceCreated(d3d, adapter, type, flags, pp, *out);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hkExCreateDevice(IDirect3D9Ex* d3d, UINT adapter, D3DDEVTYPE type,
                                           HWND wnd, DWORD flags, D3DPRESENT_PARAMETERS* pp,
                                           IDirect3DDevice9** out) {
    HRESULT hr = g_d3dex.CreateDevice(d3d, adapter, type, wnd, flags, pp, out);
    profiler::Log("CreateDevice (9Ex) -> 0x%08lX", static_cast<unsigned long>(hr));
    if (SUCCEEDED(hr) && out && *out) {
        OnDeviceCreated(reinterpret_cast<IDirect3D9*>(d3d), adapter, type, flags, pp, *out);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hkCreateDeviceEx(IDirect3D9Ex* d3d, UINT adapter, D3DDEVTYPE type,
                                           HWND wnd, DWORD flags, D3DPRESENT_PARAMETERS* pp,
                                           D3DDISPLAYMODEEX* mode, IDirect3DDevice9Ex** out) {
    HRESULT hr = g_d3dex.CreateDeviceEx(d3d, adapter, type, wnd, flags, pp, mode, out);
    profiler::Log("CreateDeviceEx -> 0x%08lX (PresentEx is not hooked)",
               static_cast<unsigned long>(hr));
    if (SUCCEEDED(hr) && out && *out) {
        OnDeviceCreated(reinterpret_cast<IDirect3D9*>(d3d), adapter, type, flags, pp,
                        reinterpret_cast<IDirect3DDevice9*>(*out));
    }
    return hr;
}

}  // namespace

void PatchD3D9(IDirect3D9* d3d) {
    Patch(&d3d->lpVtbl->CreateDevice, hkCreateDevice, &g_d3d.CreateDevice, "CreateDevice");
}

void PatchD3D9Ex(IDirect3D9Ex* d3d) {
    Patch(&d3d->lpVtbl->CreateDevice, hkExCreateDevice, &g_d3dex.CreateDevice, "CreateDevice (9Ex)");
    Patch(&d3d->lpVtbl->CreateDeviceEx, hkCreateDeviceEx, &g_d3dex.CreateDeviceEx, "CreateDeviceEx");
}

}  // namespace hooks
