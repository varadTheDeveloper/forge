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
///   * Phase 8.2: process mitigation policies (SetProcessMitigationPolicy):
///       - image loading: no DLLs from network shares or from files with a
///         low integrity label, and System32 first for same-named DLLs;
///       - extension points off: no AppInit_DLLs, legacy IME or global
///         window-hook DLLs are injected into Forge.
///     Not enabled: Arbitrary Code Guard (it blocks the JIT compiler),
///     Microsoft-signed-only images (mozglue.dll is not Microsoft-signed),
///     strict handle checks and a child-process block (both planned with
///     the OS sandbox, Phase 13).
/// Every step is best effort: a failure leaves Windows' default
/// behaviour in place rather than stopping Forge from starting.
void HardenProcess() noexcept;

/// Which of HardenProcess()'s mitigation policies are in force, read back
/// from Windows (for the self-test).
struct MitigationStatus
{
    bool noRemoteImages = false;
    bool noLowLabelImages = false;
    bool preferSystem32Images = false;
    bool extensionPointsDisabled = false;
};

[[nodiscard]]
MitigationStatus QueryMitigations() noexcept;

/// Total physical memory in bytes, or 0 if it cannot be determined.
/// Used to size the garbage collector's heuristics.
[[nodiscard]]
u64 PhysicalMemoryBytes() noexcept;

} // namespace forge::core::platform
