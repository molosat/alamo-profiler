// Windows smoke test for the proxy. Put smoke_d3d9.exe next to the built
// d3d9.dll and run it: it renders 300 frames through the proxy, then checks
// that the CSV has one row per frame with the expected counters and that the
// device still releases to 0 (the GPU queries must not keep it alive).
#include <windows.h>
#include <d3d9.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sstream>
#include <vector>

namespace {

constexpr int kFrames = 300;
std::atomic<int> g_failures{0};

void Check(bool ok, const char* what) {
    printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

std::wstring ExeDir() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring s(buf, n);
    return s.substr(0, s.find_last_of(L"\\/"));
}

std::wstring NewestCsv(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*_frames.csv").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return L"";
    std::wstring best;
    FILETIME best_time = {};
    do {
        if (CompareFileTime(&fd.ftLastWriteTime, &best_time) >= 0) {
            best_time = fd.ftLastWriteTime;
            best = dir + L"\\" + fd.cFileName;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return best;
}

std::vector<std::string> Split(const std::string& s) {
    std::vector<std::string> out;
    size_t start = 0;
    for (;;) {
        size_t comma = s.find(',', start);
        out.push_back(s.substr(start, comma - start));
        if (comma == std::string::npos) return out;
        start = comma + 1;
    }
}

struct Vertex {
    float x, y, z, rhw;
    DWORD color;
};

std::string ReadFile(const std::wstring& path) {
    std::string s;
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return s;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
    fclose(f);
    return s;
}

int FileCount(const std::wstring& pattern) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int count = 0;
    do { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) ++count; }
    while (FindNextFileW(h, &fd));
    FindClose(h);
    return count;
}

class SmokeIni {
public:
    explicit SmokeIni(const std::wstring& dir, bool csv = true, bool release_test = false,
                      int memory_every = 60,
                      const std::string& output_dir = "perf_logs")
        : path_(dir + L"\\alamo_profiler.ini") {
        existed_ = GetFileAttributesW(path_.c_str()) != INVALID_FILE_ATTRIBUTES;
        if (existed_) original_ = ReadFile(path_);
        FILE* f = _wfopen(path_.c_str(), L"wb");
        if (f) {
            const std::string text = std::string("[alamo_profiler]\nEnabled=1\nOutputDir=") + output_dir +
                "\nMemoryEvery=" + std::to_string(memory_every) + "\nFramesCsv=" +
                (csv ? "1" : "0") + (release_test ?
                    "\nGpuTiming=0\nFlushEvery=10000\n" : "\n");
            ready = fwrite(text.data(), 1, text.size(), f) == text.size();
            fclose(f);
        }
    }
    void Remove() { DeleteFileW(path_.c_str()); }
    ~SmokeIni() {
        Remove();
        if (existed_) {
            FILE* f = _wfopen(path_.c_str(), L"wb");
            if (f) {
                fwrite(original_.data(), 1, original_.size(), f);
                fclose(f);
            }
        }
    }
    bool ready = false;
private:
    std::wstring path_;
    std::string original_;
    bool existed_ = false;
};

// Child mode: render a few frames, then end the process the way the game
// does, with TerminateProcess on itself, which skips DLL_PROCESS_DETACH.
constexpr int kChildFrames = 50;

bool ReleasedRows(const std::wstring& csv, int first, int second) {
    std::istringstream input(ReadFile(csv));
    std::string line;
    if (!std::getline(input, line) || line.find("frame,t_ms,") != 0) return false;
    int rows = 0;
    double previous_time = -1;
    while (std::getline(input, line)) {
        auto columns = Split(line);
        if (columns.size() < 18 || columns[0] != std::to_string(rows + 1) ||
            columns[15] != (rows < first ? "1" : "2") || !columns[6].empty()) return false;
        const double time = std::strtod(columns[1].c_str(), nullptr);
        if (time < previous_time) return false;
        previous_time = time;
        ++rows;
    }
    return rows == first + second;
}

int ReleaseFlushesCsvWithoutGpuTiming(const std::wstring& dir) {
    SmokeIni ini(dir, true, true);
    if (!ini.ready) return 2;
    HMODULE proxy = LoadLibraryW((dir + L"\\d3d9.dll").c_str());
    if (!proxy) return 3;
    using Create = IDirect3D9*(WINAPI*)(UINT);
    auto create = reinterpret_cast<Create>(reinterpret_cast<void*>(GetProcAddress(proxy, "Direct3DCreate9")));
    IDirect3D9* d3d = create ? create(D3D_SDK_VERSION) : nullptr;
    if (!d3d) return 4;
    HWND wnd = CreateWindowW(L"STATIC", L"release CSV", WS_OVERLAPPEDWINDOW,
                            0, 0, 320, 240, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    bool ok = wnd != nullptr;
    std::wstring csv;
    for (int device = 0; device < 2 && ok; ++device) {
        D3DPRESENT_PARAMETERS pp = {};
        pp.Windowed = TRUE;
        pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
        pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
        IDirect3DDevice9* dev = nullptr;
        ok = SUCCEEDED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, wnd,
            D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev)) && dev;
        if (!ok) break;
        for (int frame = 0; frame < (device ? 5 : 7); ++frame) {
            for (int clear = 0; clear <= device; ++clear)
                ok = SUCCEEDED(dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0xff202020, 1.0f, 0)) && ok;
            ok = SUCCEEDED(dev->Present(nullptr, nullptr, nullptr, nullptr)) && ok;
        }
        ok = dev->Release() == 0 && ok;
        if (!device) csv = NewestCsv(dir + L"\\perf_logs");
        // Check before FreeLibrary, with fewer frames than FlushEvery. The
        // second check also proves device recreation preserves CSV ordering.
        ok = ReleasedRows(csv, 6, device ? 5 : 0) && ok;
        Check(ok, device ? "ReleaseFlushesCsvWithoutGpuTiming second device rows in order" :
                           "ReleaseFlushesCsvWithoutGpuTiming first device flushes before FreeLibrary");
    }
    d3d->Release();
    if (wnd) DestroyWindow(wnd);
    FreeLibrary(proxy);
    return ok ? 0 : 5;
}

// A failed Reset must not end GPU timing: the retry that succeeds attaches it again.
int FailedResetKeepsGpuTiming(const std::wstring& dir) {
    SmokeIni ini(dir);
    if (!ini.ready) return 2;
    HMODULE proxy = LoadLibraryW((dir + L"\\d3d9.dll").c_str());
    if (!proxy) return 3;
    using Create = IDirect3D9*(WINAPI*)(UINT);
    auto create = reinterpret_cast<Create>(reinterpret_cast<void*>(GetProcAddress(proxy, "Direct3DCreate9")));
    IDirect3D9* d3d = create ? create(D3D_SDK_VERSION) : nullptr;
    if (!d3d) return 4;
    HWND wnd = CreateWindowW(L"STATIC", L"failed reset", WS_OVERLAPPEDWINDOW,
                            0, 0, 320, 240, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    D3DPRESENT_PARAMETERS pp = {};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    IDirect3DDevice9* dev = nullptr;
    if (!wnd || FAILED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, wnd,
            D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev)) || !dev) return 5;
    const auto render = [&](int frames) {
        for (int i = 0; i < frames; ++i) {
            dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0xff202020, 1.0f, 0);
            dev->BeginScene();
            dev->EndScene();
            dev->Present(nullptr, nullptr, nullptr, nullptr);
            Sleep(2);
        }
    };
    // The DLL runs without timestamp queries on drivers that lack them.
    const bool has_timestamps = SUCCEEDED(dev->CreateQuery(D3DQUERYTYPE_TIMESTAMP, nullptr)) &&
        SUCCEEDED(dev->CreateQuery(D3DQUERYTYPE_TIMESTAMPDISJOINT, nullptr)) &&
        SUCCEEDED(dev->CreateQuery(D3DQUERYTYPE_TIMESTAMPFREQ, nullptr));
    render(5);
    D3DPRESENT_PARAMETERS bad = pp;
    bad.SwapEffect = static_cast<D3DSWAPEFFECT>(0);
    const bool failed = FAILED(dev->Reset(&bad));
    Check(failed, "bad Reset fails");
    const bool retried = SUCCEEDED(dev->Reset(&pp));
    Check(retried, "Reset retry succeeds");
    render(40);
    const bool released = dev->Release() == 0;
    std::istringstream input(ReadFile(NewestCsv(dir + L"\\perf_logs")));
    std::string line;
    int rows = 0, late_gpu = 0;
    std::getline(input, line);
    while (std::getline(input, line)) {
        auto columns = Split(line);
        if (++rows > 25 && columns.size() > 6 && !columns[6].empty()) ++late_gpu;
    }
    printf("INFO  rows %d, gpu_ms after the retry on %d\n", rows, late_gpu);
    if (has_timestamps) Check(late_gpu > 0, "gpu_ms returns after a failed Reset");
    else printf("SKIP  gpu_ms after a failed Reset: no timestamp queries\n");
    d3d->Release();
    DestroyWindow(wnd);
    FreeLibrary(proxy);
    return failed && retried && released && (late_gpu > 0 || !has_timestamps) ? 0 : 6;
}

using SwapGetDevice = HRESULT(STDMETHODCALLTYPE*)(IDirect3DSwapChain9*, IDirect3DDevice9**);
SwapGetDevice g_swap_get_device = nullptr;
unsigned g_swap_get_device_calls = 0;
HRESULT STDMETHODCALLTYPE CountSwapGetDevice(IDirect3DSwapChain9* chain, IDirect3DDevice9** device) {
    ++g_swap_get_device_calls;
    return g_swap_get_device(chain, device);
}

bool SwapPresentAvoidsGetDevice(IDirect3DDevice9* device) {
    IDirect3DSwapChain9* chain = nullptr;
    if (FAILED(device->GetSwapChain(0, &chain))) return false;
    // IUnknown (3), Present, front/back buffers, raster/display, GetDevice.
    auto table = *reinterpret_cast<void***>(chain);
    DWORD old = 0;
    if (!VirtualProtect(&table[8], sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) {
        chain->Release(); return false;
    }
    g_swap_get_device = reinterpret_cast<SwapGetDevice>(table[8]);
    table[8] = reinterpret_cast<void*>(&CountSwapGetDevice);
    VirtualProtect(&table[8], sizeof(void*), old, &old);
    const HRESULT hr = chain->Present(nullptr, nullptr, nullptr, nullptr, 0);
    DWORD ignored;
    VirtualProtect(&table[8], sizeof(void*), PAGE_EXECUTE_READWRITE, &ignored);
    table[8] = reinterpret_cast<void*>(g_swap_get_device);
    VirtualProtect(&table[8], sizeof(void*), old, &ignored);
    chain->Release();
    return SUCCEEDED(hr) && g_swap_get_device_calls == 0;
}

int TerminateChild(const std::wstring& dir, bool csv_off = false) {
    HMODULE proxy = LoadLibraryW((dir + L"\\d3d9.dll").c_str());
    if (!proxy) return 2;
    using CreateFn = IDirect3D9*(WINAPI*)(UINT);
    auto create = reinterpret_cast<CreateFn>(
        reinterpret_cast<void*>(GetProcAddress(proxy, "Direct3DCreate9")));
    IDirect3D9* d3d = create ? create(D3D_SDK_VERSION) : nullptr;
    if (!d3d) return 3;
    WNDCLASSW wc = {};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"alamo_profiler_smoke_child";
    RegisterClassW(&wc);
    HWND wnd = CreateWindowW(wc.lpszClassName, L"smoke child", WS_OVERLAPPEDWINDOW, 0, 0, 320,
                             240, nullptr, nullptr, wc.hInstance, nullptr);
    D3DPRESENT_PARAMETERS pp = {};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    IDirect3DDevice9* dev = nullptr;
    const HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, wnd,
                                        D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev);
    if (FAILED(hr)) {
        printf("FAIL child CreateDevice: 0x%08lX\n", static_cast<unsigned long>(hr));
        return 4;
    }
    const bool swap_ok = csv_off || SwapPresentAvoidsGetDevice(dev);
    printf("%s swap Present skips GetDevice\n", swap_ok ? "PASS" : "FAIL");
    fflush(stdout);
    if (!swap_ok) return 9;
    // The swap-chain check already presented the origin frame.
    const int child_frames = csv_off ? kChildFrames : kChildFrames - 1;
    for (int i = 0; i < child_frames; ++i) {
        dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0xff202020, 1.0f, 0);
        dev->BeginScene();
        dev->EndScene();
        dev->Present(nullptr, nullptr, nullptr, nullptr);
    }
    if (csv_off) {
        const bool ok = dev->Release() == 0;
        d3d->Release();
        DestroyWindow(wnd);
        FreeLibrary(proxy);
        printf("%s CSV disabled rendering\n", ok ? "PASS" : "FAIL");
        return ok ? 0 : 6;
    }
    TerminateProcess(GetCurrentProcess(), 0);
    return 5;
}

}  // namespace

int main(int argc, char** argv) {
    std::wstring dir = ExeDir();
    if (argc > 1 && strcmp(argv[1], "--terminate-child") == 0) return TerminateChild(dir);
    if (argc > 1 && strcmp(argv[1], "--csv-off-child") == 0) return TerminateChild(dir, true);
    if (argc > 1 && strcmp(argv[1], "--release-csv-child") == 0)
        return ReleaseFlushesCsvWithoutGpuTiming(dir);
    if (argc > 1 && strcmp(argv[1], "--failed-reset-child") == 0)
        return FailedResetKeepsGpuTiming(dir);
    SmokeIni ini(dir);
    Check(ini.ready, "temporary timing ini written");
    if (!ini.ready) return 1;
    HMODULE proxy = LoadLibraryW((dir + L"\\d3d9.dll").c_str());
    Check(proxy != nullptr, "proxy d3d9.dll loads");
    if (!proxy) return 1;
    using CreateFn = IDirect3D9*(WINAPI*)(UINT);
    auto create = reinterpret_cast<CreateFn>(
        reinterpret_cast<void*>(GetProcAddress(proxy, "Direct3DCreate9")));
    Check(create != nullptr, "Direct3DCreate9 exported");
    if (!create) return 1;

    WNDCLASSW wc = {};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"alamo_profiler_smoke";
    RegisterClassW(&wc);
    HWND wnd = CreateWindowW(wc.lpszClassName, L"Alamo Profiler smoke", WS_OVERLAPPEDWINDOW, 0, 0,
                             320, 240, nullptr, nullptr, wc.hInstance, nullptr);

    IDirect3D9* d3d = create(D3D_SDK_VERSION);
    Check(d3d != nullptr, "Direct3DCreate9 returns an object");
    if (!d3d) return 1;
    D3DPRESENT_PARAMETERS pp = {};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_UNKNOWN;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    IDirect3DDevice9* dev = nullptr;
    HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, wnd,
                                   D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev);
    if (FAILED(hr))
        hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, wnd,
                               D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev);
    Check(SUCCEEDED(hr) && dev, "CreateDevice through the hook");
    if (!dev) {
        printf("INFO  CreateDevice HRESULT 0x%08lX\n", static_cast<unsigned long>(hr));
        d3d->Release();
        DestroyWindow(wnd);
        FreeLibrary(proxy);
        return 1;
    }

    const Vertex tri[3] = {{160, 20, 0.5f, 1, 0xffff8000},
                           {280, 220, 0.5f, 1, 0xff00ff80},
                           {40, 220, 0.5f, 1, 0xff0080ff}};
    dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    // Record a state block, as D3DX effects do while the game loads. The
    // runtime rewrites the device vtable around the recording.
    IDirect3DStateBlock9* sb = nullptr;
    dev->BeginStateBlock();
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->EndStateBlock(&sb);
    Check(sb != nullptr, "state block recorded");
    int rendered = 0;
    const ULONGLONG render_deadline = GetTickCount64() + 30000;
    for (; rendered < kFrames && GetTickCount64() < render_deadline; ++rendered) {
        MSG message;
        while (PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0xff202020, 1.0f, 0);
        dev->BeginScene();
        dev->SetRenderState(D3DRS_LIGHTING, FALSE);
        dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, tri, sizeof(Vertex));
        dev->EndScene();
        dev->Present(nullptr, nullptr, nullptr, nullptr);
        Sleep(1);  // Give the background memory sampler time to run.
    }
    if (sb) sb->Release();
    ULONG dev_refs = dev->Release();
    Check(dev_refs == 0, "device Release returns 0 with GPU queries attached");
    d3d->Release();
    DestroyWindow(wnd);
    ini.Remove();
    FreeLibrary(proxy);  // Device release must have flushed even when the writer pins the DLL.
    Check(GetModuleHandleW((dir + L"\\d3d9.dll").c_str()) != nullptr,
          "session writer pins module across FreeLibrary");

    std::wstring csv = NewestCsv(dir + L"\\perf_logs");
    Check(!csv.empty(), "frames CSV written to perf_logs");
    if (csv.empty()) return 1;
    FILE* f = _wfopen(csv.c_str(), L"r");
    char line[4096];
    std::vector<std::string> head;
    if (f && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        head = Split(line);
    }
    auto col = [&](const char* name) {
        for (size_t i = 0; i < head.size(); ++i)
            if (head[i] == name) return static_cast<int>(i);
        return -1;
    };
    int c_draws = col("draws"), c_clears = col("clears"), c_scenes = col("scenes"),
        c_prims = col("prims"), c_gpu = col("gpu_ms"), c_rs = col("set_render_state"),
        c_t_ms = col("t_ms");
    Check(c_draws >= 0 && c_clears >= 0 && c_scenes >= 0 && c_gpu >= 0, "CSV header has columns");
    int rows = 0, bad = 0, with_gpu = 0;
    std::vector<std::string> frame_times;
    while (f && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        std::vector<std::string> r = Split(line);
        ++rows;
        if (r.size() != head.size() || r[c_draws] != "1" || r[c_prims] != "1" ||
            r[c_clears] != "1" || r[c_scenes] != "1" || r[c_rs] != "1")
            ++bad;
        if (!r[c_gpu].empty()) ++with_gpu;
        frame_times.push_back(c_t_ms >= 0 && r.size() == head.size() ? r[c_t_ms] : "");
    }
    if (f) fclose(f);
    printf("rows=%d bad=%d with_gpu=%d\n", rows, bad, with_gpu);
    Check(rows == rendered - 1, "one row per frame after the first Present");
    Check(bad == 0, "every row counts 1 draw/prim/clear/scene/render state");
    // Not a failure: some drivers (and Wine) lack timestamp queries.
    printf("INFO  gpu_ms present on %d of %d rows\n", with_gpu, rows);

    const auto memory_path = csv.substr(0, csv.size() - wcslen(L"_frames.csv")) + L"_memory.csv";
    bool memory_header = false, memory_row = false, memory_join = true;
    double memory_max_ms = 0;
    const ULONGLONG memory_deadline = GetTickCount64() + 2000;
    do {
        FILE* memory_file = _wfopen(memory_path.c_str(), L"r");
        if (memory_file) {
            if (fgets(line, sizeof line, memory_file)) {
                line[strcspn(line, "\r\n")] = 0;
                memory_header = strcmp(line,
                    "frame,t_ms,private_bytes,largest_free_bytes,regions,truncated,sample_ms") == 0;
            }
            while (fgets(line, sizeof line, memory_file)) {
                const auto sample = Split(line);
                if (sample.size() != 7) continue;
                const auto frame = strtoull(sample[0].c_str(), nullptr, 10);
                memory_join = memory_join && frame > 0 && frame <= static_cast<unsigned>(rows) && frame % 60 == 0;
                if (frame > 0 && frame <= frame_times.size())
                    memory_join = memory_join && sample[1] == frame_times[static_cast<size_t>(frame - 1)];
                memory_row = memory_row || (strtoull(sample[2].c_str(), nullptr, 10) > 0 &&
                                            strtoull(sample[3].c_str(), nullptr, 10) > 0);
                const double cost = strtod(sample[6].c_str(), nullptr);
                if (cost > memory_max_ms) memory_max_ms = cost;
            }
            fclose(memory_file);
        }
        if (memory_row) break;
        Sleep(20);
    } while (GetTickCount64() < memory_deadline);
    Check(memory_header, "memory sidecar has exact header");
    Check(memory_row, "memory sidecar has positive private bytes and largest free block");
    Check(memory_row && memory_join, "memory samples identify rendered frames at the default interval");
    printf("INFO  memory sample_ms max %.3f\n", memory_max_ms);

    // The game exits with TerminateProcess; the profiler must still write
    // everything and log its shutdown line.
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring cmd = L"\"" + std::wstring(exe) + L"\" --terminate-child";
    Sleep(1100);  // the child's log files are named by the second
    STARTUPINFOW si = {sizeof si};
    PROCESS_INFORMATION pi = {};
    SECURITY_ATTRIBUTES sa = {sizeof sa, nullptr, TRUE};
    HANDLE child_out = nullptr, child_write = nullptr;
    bool output_pipe = CreatePipe(&child_out, &child_write, &sa, 0) != 0;
    if (output_pipe) {
        SetHandleInformation(child_out, HANDLE_FLAG_INHERIT, 0);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = child_write;
        si.hStdError = child_write;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    }
    bool started = output_pipe && CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, TRUE, 0, nullptr,
                                  dir.c_str(), &si, &pi) != 0;
    if (child_write) CloseHandle(child_write);
    std::string child_output;
    if (started) {
        if (WaitForSingleObject(pi.hProcess, 60000) == WAIT_TIMEOUT) {
            // The proxy's exit hook patched this exe's import; the parent
            // has unloaded the DLL by now, so resolve the runtime directly.
            using TerminateFn = BOOL(WINAPI*)(HANDLE, UINT);
            auto terminate = reinterpret_cast<TerminateFn>(reinterpret_cast<void*>(
                GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "TerminateProcess")));
            if (terminate) terminate(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 5000);
        }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    if (child_out) {
        char buf[1024];
        DWORD n = 0;
        while (ReadFile(child_out, buf, sizeof buf, &n, nullptr) && n)
            child_output.append(buf, n);
        CloseHandle(child_out);
    }
    Check(started && child_output.find("PASS swap Present skips GetDevice") != std::string::npos,
          "swap Present avoids GetDevice and Release");
    std::wstring child_csv = NewestCsv(dir + L"\\perf_logs");
    std::string child_rows = ReadFile(child_csv);
    int lines = 0;
    for (char ch : child_rows) lines += ch == '\n';
    std::string session =
        ReadFile(child_csv.substr(0, child_csv.size() - wcslen(L"_frames.csv")) + L"_session.txt");
    char want[64];
    snprintf(want, sizeof want, "shutdown after %d frames", kChildFrames - 1);
    printf("child rows=%d\n", lines - 1);
    Check(started && child_csv != csv && lines - 1 == kChildFrames - 1,
          "TerminateProcess exit: every frame written");
    Check(session.find(want) != std::string::npos, "TerminateProcess exit: shutdown logged");
    {
        Sleep(1100);
        std::wstring release_cmd = L"\"" + std::wstring(exe) + L"\" --release-csv-child";
        STARTUPINFOW release_si = {sizeof release_si};
        PROCESS_INFORMATION release_pi = {};
        const bool launched = CreateProcessW(nullptr, &release_cmd[0], nullptr, nullptr,
            FALSE, CREATE_NO_WINDOW, nullptr, dir.c_str(), &release_si, &release_pi) != FALSE;
        DWORD code = 1;
        if (launched) {
            if (WaitForSingleObject(release_pi.hProcess, 15000) == WAIT_OBJECT_0)
                GetExitCodeProcess(release_pi.hProcess, &code);
            else {
                using Terminate = BOOL(WINAPI*)(HANDLE, UINT);
                auto terminate = reinterpret_cast<Terminate>(reinterpret_cast<void*>(
                    GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "TerminateProcess")));
                if (terminate) terminate(release_pi.hProcess, 1);
                WaitForSingleObject(release_pi.hProcess, 5000);
            }
            CloseHandle(release_pi.hThread);
            CloseHandle(release_pi.hProcess);
        }
        Check(launched && code == 0, "ReleaseFlushesCsvWithoutGpuTiming");
    }
    {
        Sleep(1100);  // Each child's CSV needs its own second-stamped name.
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring command = L"\"" + std::wstring(exe) + L"\" --failed-reset-child";
        STARTUPINFOW si = {};
        si.cb = sizeof si;
        PROCESS_INFORMATION pi = {};
        const bool launched = CreateProcessW(exe, command.data(), nullptr, nullptr, FALSE, 0,
                                             nullptr, nullptr, &si, &pi) != 0;
        DWORD code = 1;
        if (launched) {
            if (WaitForSingleObject(pi.hProcess, 15000) == WAIT_OBJECT_0)
                GetExitCodeProcess(pi.hProcess, &code);
            else {
                using Terminate = BOOL(WINAPI*)(HANDLE, UINT);
                auto terminate = reinterpret_cast<Terminate>(reinterpret_cast<void*>(
                    GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "TerminateProcess")));
                if (terminate) terminate(pi.hProcess, 1);
                WaitForSingleObject(pi.hProcess, 5000);
            }
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
        Check(launched && code == 0, "FailedResetKeepsGpuTiming");
    }
    {
        Sleep(1100);  // A CSV regression must not overwrite the previous child's same-second file.
        SmokeIni csv_off_ini(dir, false);
        const int before = FileCount(dir + L"\\perf_logs\\*_frames.csv");
        const int memory_before = FileCount(dir + L"\\perf_logs\\*_memory.csv");
        std::wstring off_cmd = L"\"" + std::wstring(exe) + L"\" --csv-off-child";
        HANDLE read = nullptr, write = nullptr;
        bool launched = CreatePipe(&read, &write, &sa, 0) != 0;
        SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
        si.hStdOutput = write;
        si.hStdError = write;
        launched = launched && csv_off_ini.ready && CreateProcessW(nullptr, &off_cmd[0], nullptr,
            nullptr, TRUE, CREATE_NO_WINDOW, nullptr, dir.c_str(), &si, &pi) != 0;
        if (write) CloseHandle(write);
        std::string output;
        if (launched) {
            if (WaitForSingleObject(pi.hProcess, 15000) == WAIT_TIMEOUT) {
                using TerminateFn = BOOL(WINAPI*)(HANDLE, UINT);
                auto terminate = reinterpret_cast<TerminateFn>(reinterpret_cast<void*>(
                    GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "TerminateProcess")));
                if (terminate) terminate(pi.hProcess, 1);
            }
            WaitForSingleObject(pi.hProcess, 5000);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
        if (read) {
            char buf[1024]; DWORD n;
            while (ReadFile(read, buf, sizeof buf, &n, nullptr) && n) output.append(buf, n);
            CloseHandle(read);
        }
        Check(launched && output.find("PASS CSV disabled rendering") != std::string::npos &&
              FileCount(dir + L"\\perf_logs\\*_frames.csv") == before, "FramesCsv=0 renders without a frame CSV");
        Check(launched && FileCount(dir + L"\\perf_logs\\*_memory.csv") == memory_before,
              "FramesCsv=0 writes no memory sidecar");
    }
    return g_failures ? 1 : 0;
}
