// d3d9.dll exports. The game (and D3DX, which loads "d3d9.dll" by name) bind
// to this module; each export forwards to the real runtime, loaded lazily so
// nothing heavy happens under the loader lock.
#include <d3d9.h>

#include "device_hooks.h"
#include "exit_hook.h"
#include "profiler.h"

namespace {

HMODULE g_self = nullptr;
HMODULE g_real = nullptr;
INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;

BOOL CALLBACK LoadReal(PINIT_ONCE, PVOID, PVOID*) {
    profiler::Init(g_self);
    if (profiler::Cfg().enabled) exit_hook::Install();
    std::wstring path = profiler::Cfg().chain_load;
    if (path.empty()) {
        wchar_t sys[MAX_PATH];
        UINT n = GetSystemDirectoryW(sys, MAX_PATH);
        path.assign(sys, n);
        path += L"\\d3d9.dll";
    }
    g_real = LoadLibraryW(path.c_str());
    if (g_real)
        profiler::Log("forwarding to %ls", path.c_str());
    else
        profiler::Log("ERROR: could not load %ls (error %lu)", path.c_str(), GetLastError());
    return TRUE;
}

template <class F>
F Real(const char* name) {
    InitOnceExecuteOnce(&g_once, LoadReal, nullptr, nullptr);
    return g_real ? reinterpret_cast<F>(reinterpret_cast<void*>(GetProcAddress(g_real, name)))
                  : nullptr;
}

}  // namespace

extern "C" {

IDirect3D9* WINAPI Direct3DCreate9(UINT sdk_version) {
    auto fn = Real<decltype(&Direct3DCreate9)>("Direct3DCreate9");
    IDirect3D9* d3d = fn ? fn(sdk_version) : nullptr;
    profiler::Log("Direct3DCreate9(%u) -> %p", sdk_version, static_cast<void*>(d3d));
    if (d3d && profiler::Cfg().enabled) hooks::PatchD3D9(d3d);
    return d3d;
}

HRESULT WINAPI Direct3DCreate9Ex(UINT sdk_version, IDirect3D9Ex** out) {
    auto fn = Real<decltype(&Direct3DCreate9Ex)>("Direct3DCreate9Ex");
    if (!fn) return D3DERR_NOTAVAILABLE;
    HRESULT hr = fn(sdk_version, out);
    profiler::Log("Direct3DCreate9Ex(%u) -> 0x%08lX", sdk_version, static_cast<unsigned long>(hr));
    if (SUCCEEDED(hr) && out && *out && profiler::Cfg().enabled) hooks::PatchD3D9Ex(*out);
    return hr;
}

// PIX markers. D3DX calls these; the game may too.
int WINAPI D3DPERF_BeginEvent(D3DCOLOR color, const WCHAR* name) {
    auto fn = Real<decltype(&D3DPERF_BeginEvent)>("D3DPERF_BeginEvent");
    return fn ? fn(color, name) : 0;
}

int WINAPI D3DPERF_EndEvent(void) {
    auto fn = Real<decltype(&D3DPERF_EndEvent)>("D3DPERF_EndEvent");
    return fn ? fn() : 0;
}

DWORD WINAPI D3DPERF_GetStatus(void) {
    auto fn = Real<decltype(&D3DPERF_GetStatus)>("D3DPERF_GetStatus");
    return fn ? fn() : 0;
}

BOOL WINAPI D3DPERF_QueryRepeatFrame(void) {
    auto fn = Real<decltype(&D3DPERF_QueryRepeatFrame)>("D3DPERF_QueryRepeatFrame");
    return fn ? fn() : FALSE;
}

void WINAPI D3DPERF_SetMarker(D3DCOLOR color, const WCHAR* name) {
    auto fn = Real<decltype(&D3DPERF_SetMarker)>("D3DPERF_SetMarker");
    if (fn) fn(color, name);
}

void WINAPI D3DPERF_SetOptions(DWORD options) {
    auto fn = Real<decltype(&D3DPERF_SetOptions)>("D3DPERF_SetOptions");
    if (fn) fn(options);
}

void WINAPI D3DPERF_SetRegion(D3DCOLOR color, const WCHAR* name) {
    auto fn = Real<decltype(&D3DPERF_SetRegion)>("D3DPERF_SetRegion");
    if (fn) fn(color, name);
}

// Undocumented exports some D3DX versions import. Their calling conventions are
// known from the SDK import libraries; they are forwarded untouched.
void* WINAPI Direct3DShaderValidatorCreate9(void) {
    auto fn = Real<void*(WINAPI*)(void)>("Direct3DShaderValidatorCreate9");
    return fn ? fn() : nullptr;
}

void WINAPI DebugSetMute(void) {
    auto fn = Real<void(WINAPI*)(void)>("DebugSetMute");
    if (fn) fn();
}

}  // extern "C"

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = inst;
        DisableThreadLibraryCalls(inst);
    } else if (reason == DLL_PROCESS_DETACH) {
        profiler::Shutdown();
        // The real d3d9 is not freed: at process exit it may already be gone,
        // and at FreeLibrary time the game has released its objects anyway.
    }
    return TRUE;
}
