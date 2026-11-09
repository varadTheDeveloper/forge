#pragma once

// Windows-only, like every other OS-facing component in forge-core today.
#if !defined(_WIN32)
#error "forge::core::platform::Process: Windows-only."
#endif

#include "../Types.h"

namespace forge::core::platform
{

/// Process-wide hardening, applied once at the very start of main()
/// (Phase 7.9, rule R22), before anything can load a DLL:
///   * DLLs loaded at run time come only from System32 and the folder
///     that contains forge.exe (SetDefaultDllDirectories), never from
///     the current directory or PATH. This blocks DLL planting.
///   * The current directory is removed from the DLL search path
///     (SetDllDirectoryW(L"")) and safe search mode is made permanent
///     for SearchPath (SetSearchPathMode).
/// Every step is best effort: a failure leaves Windows' default
/// behaviour in place rather than stopping Forge from starting.
void HardenProcess() noexcept;

/// Total physical memory in bytes, or 0 if it cannot be determined.
/// Used to size the garbage collector's heuristics.
[[nodiscard]]
u64 PhysicalMemoryBytes() noexcept;

} // namespace forge::core::platform
