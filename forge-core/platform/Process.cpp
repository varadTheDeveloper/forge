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
