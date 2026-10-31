// Linked into every Windows executable this project builds. A crashing or
// asserting test must fail with an exit code, not wait on a modal error
// dialog on the owner's desktop (agents run these tests unattended).
#include <windows.h>

#include <stdlib.h>
#ifdef _MSC_VER
#include <crtdbg.h>
#endif

namespace {
const bool kCrashDialogsOff = [] {
    SetErrorMode(GetErrorMode() | SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX |
                 SEM_NOOPENFILEERRORBOX);
#ifdef _MSC_VER
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    const int kReportTypes[] = {_CRT_WARN, _CRT_ERROR, _CRT_ASSERT};
    for (int type : kReportTypes) {
        _CrtSetReportMode(type, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
        _CrtSetReportFile(type, _CRTDBG_FILE_STDERR);
    }
#endif
    return true;
}();
}  // namespace
