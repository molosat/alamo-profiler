#include "exit_hook.h"

#include <windows.h>

#include <cstring>

#include "profiler.h"

namespace exit_hook {
namespace {

using TerminateFn = BOOL(WINAPI*)(HANDLE, UINT);
TerminateFn g_orig = nullptr;
bool g_installed = false;

BOOL WINAPI HookTerminateProcess(HANDLE process, UINT code) {
    if (process == GetCurrentProcess() || GetProcessId(process) == GetCurrentProcessId()) {
        profiler::Log("TerminateProcess(self, %u)", code);
        profiler::Shutdown();
    }
    return g_orig(process, code);
}

// Finds the exe's import slot for kernel32!TerminateProcess, by name where the
// import has a name table, else by the resolved address.
FARPROC* FindSlot(HMODULE exe, FARPROC target) {
    auto* base = reinterpret_cast<BYTE*>(exe);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& dir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return nullptr;
    for (auto* d = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
         d->Name; ++d) {
        auto* iat = reinterpret_cast<IMAGE_THUNK_DATA*>(base + d->FirstThunk);
        auto* names = d->OriginalFirstThunk
                          ? reinterpret_cast<IMAGE_THUNK_DATA*>(base + d->OriginalFirstThunk)
                          : nullptr;
        for (size_t i = 0; iat[i].u1.Function; ++i) {
            bool match = reinterpret_cast<FARPROC>(iat[i].u1.Function) == target;
            if (!match && names && !IMAGE_SNAP_BY_ORDINAL(names[i].u1.Ordinal)) {
                auto* by_name =
                    reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names[i].u1.AddressOfData);
                match = strcmp(reinterpret_cast<const char*>(by_name->Name),
                               "TerminateProcess") == 0;
            }
            if (match) return reinterpret_cast<FARPROC*>(&iat[i].u1.Function);
        }
    }
    return nullptr;
}

}  // namespace

void Install() {
    if (g_installed) return;
    g_installed = true;
    FARPROC target = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "TerminateProcess");
    FARPROC* slot = FindSlot(GetModuleHandleW(nullptr), target);
    if (!slot) {
        profiler::Log("exit hook: exe does not import TerminateProcess");
        return;
    }
    g_orig = reinterpret_cast<TerminateFn>(*slot);
    DWORD old;
    if (!VirtualProtect(slot, sizeof *slot, PAGE_READWRITE, &old)) {
        profiler::Log("ERROR: exit hook VirtualProtect failed (%lu)", GetLastError());
        return;
    }
    *slot = reinterpret_cast<FARPROC>(&HookTerminateProcess);
    VirtualProtect(slot, sizeof *slot, old, &old);
    profiler::Log("exit hook: TerminateProcess import patched");
}

}  // namespace exit_hook
