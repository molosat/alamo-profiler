// The game ends with TerminateProcess on itself, which skips
// DLL_PROCESS_DETACH, so the profiler would lose its last rows and its shutdown
// line. This patches the game exe's import of TerminateProcess to shut the
// profiler down first.
#pragma once

namespace exit_hook {

// Patches the main exe's TerminateProcess import. Idempotent.
void Install();

}  // namespace exit_hook
