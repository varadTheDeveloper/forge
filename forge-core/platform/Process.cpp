#include "Process.h"

#if !defined(WIN32_LEAN_AND_MEAN)
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace forge::core::platform
{

void HardenProcess() noexcept
{
    // Run-time LoadLibrary calls search only System32 and forge.exe's own
    // folder. (Static imports are resolved by the loader before main()
    // and are unaffected.) Available since Windows 8, so always present on
    // the Windows 11 / Server 2022 minimum (decision D7).
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_APPLICATION_DIR);

    // Remove the current directory from the legacy DLL search order too.
    SetDllDirectoryW(L"");

    // SearchPath(): search the current directory last, permanently.
    SetSearchPathMode(BASE_SEARCH_PATH_ENABLE_SAFE_SEARCHMODE | BASE_SEARCH_PATH_PERMANENT);

    // Phase 8.2: mitigation policies. They only tighten, and Windows keeps
    // them for the rest of the process's life. Static imports (mozglue.dll)
    // were loaded before main() and are unaffected.
    PROCESS_MITIGATION_IMAGE_LOAD_POLICY imageLoad{};
    imageLoad.NoRemoteImages = 1;
    imageLoad.NoLowMandatoryLabelImages = 1;
    imageLoad.PreferSystem32Images = 1;
    SetProcessMitigationPolicy(ProcessImageLoadPolicy, &imageLoad, sizeof(imageLoad));

    PROCESS_MITIGATION_EXTENSION_POINT_DISABLE_POLICY extensionPoints{};
    extensionPoints.DisableExtensionPoints = 1;
    SetProcessMitigationPolicy(ProcessExtensionPointDisablePolicy, &extensionPoints,
                               sizeof(extensionPoints));
}

MitigationStatus QueryMitigations() noexcept
{
    MitigationStatus status;
    HANDLE self = GetCurrentProcess();

    PROCESS_MITIGATION_IMAGE_LOAD_POLICY imageLoad{};

    if (GetProcessMitigationPolicy(self, ProcessImageLoadPolicy, &imageLoad, sizeof(imageLoad)))
    {
        status.noRemoteImages = imageLoad.NoRemoteImages != 0;
        status.noLowLabelImages = imageLoad.NoLowMandatoryLabelImages != 0;
        status.preferSystem32Images = imageLoad.PreferSystem32Images != 0;
    }

    PROCESS_MITIGATION_EXTENSION_POINT_DISABLE_POLICY extensionPoints{};

    if (GetProcessMitigationPolicy(self, ProcessExtensionPointDisablePolicy, &extensionPoints,
                                   sizeof(extensionPoints)))
    {
        status.extensionPointsDisabled = extensionPoints.DisableExtensionPoints != 0;
    }

    return status;
}

u64 PhysicalMemoryBytes() noexcept
{
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);

    if (!GlobalMemoryStatusEx(&status))
    {
        return 0;
    }

    return static_cast<u64>(status.ullTotalPhys);
}

} // namespace forge::core::platform
