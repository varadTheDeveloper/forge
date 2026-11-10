#include "Stdio.h"

#include <stdio.h>

#include <atomic>

#if !defined(WIN32_LEAN_AND_MEAN)
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "Win32Error.h"

namespace forge::core::platform
{

namespace
{

/// Largest single WriteFile request; WriteFile takes a DWORD length.
constexpr Size kMaxFileChunk = Size{ 1 } << 30;

/// UTF-8 bytes converted per WriteConsoleW call. The UTF-16 result is
/// never longer (in code units) than the UTF-8 input (in bytes).
constexpr Size kConsoleChunk = 8192;

HANDLE StdHandle(
    StdStream stream) noexcept
{
    return GetStdHandle(stream == StdStream::Out ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE);
}

bool IsConsoleHandle(
    HANDLE handle) noexcept
{
    DWORD mode = 0;
    return handle != nullptr && handle != INVALID_HANDLE_VALUE && GetConsoleMode(handle, &mode) != 0;
}

/// Length of the longest prefix of data[0, size) that does not end in the
/// middle of a UTF-8 sequence. `size` is at most kConsoleChunk.
Size CompleteUtf8Prefix(
    const char* data,
    Size size) noexcept
{
    // Look back at most 3 bytes for the lead byte of the last sequence.
    Size back = 0;

    while (back < 3 && back < size &&
           (static_cast<unsigned char>(data[size - 1 - back]) & 0xC0) == 0x80)
    {
        ++back;
    }

    if (back == size)
    {
        return size;  // only continuation bytes: invalid anyway, write as is
    }

    const unsigned char lead = static_cast<unsigned char>(data[size - 1 - back]);
    Size needed = 1;

    if (lead >= 0xF0)
    {
        needed = 4;
    }
    else if (lead >= 0xE0)
    {
        needed = 3;
    }
    else if (lead >= 0xC0)
    {
        needed = 2;
    }

    if (needed > back + 1)
    {
        return size - 1 - back;  // the last sequence is cut: leave it out
    }

    return size;
}

Result<void> WriteConsoleUtf8(
    HANDLE handle,
    const char* data,
    Size size) noexcept
{
    wchar_t wide[kConsoleChunk];
    Size offset = 0;

    while (offset < size)
    {
        Size count = size - offset;

        if (count > kConsoleChunk)
        {
            count = CompleteUtf8Prefix(data + offset, kConsoleChunk);

            if (count == 0)
            {
                count = kConsoleChunk;
            }
        }

        const int units = MultiByteToWideChar(
            CP_UTF8, 0, data + offset, static_cast<int>(count), wide,
            static_cast<int>(kConsoleChunk));

        if (units <= 0)
        {
            return Result<void>(Failure{ TranslateWin32Error(GetLastError()) });
        }

        DWORD done = 0;

        while (done < static_cast<DWORD>(units))
        {
            DWORD written = 0;

            if (!WriteConsoleW(handle, wide + done, static_cast<DWORD>(units) - done, &written,
                               nullptr))
            {
                return Result<void>(Failure{ TranslateWin32Error(GetLastError()) });
            }

            if (written == 0)
            {
                return Result<void>(Failure{ Error(ErrorCode::IOError) });
            }

            done += written;
        }

        offset += count;
    }

    return Result<void>();
}

Result<void> WriteFileAll(
    HANDLE handle,
    const char* data,
    Size size) noexcept
{
    Size offset = 0;

    while (offset < size)
    {
        const Size remaining = size - offset;
        const DWORD request =
            static_cast<DWORD>(remaining > kMaxFileChunk ? kMaxFileChunk : remaining);
        DWORD written = 0;

        if (!WriteFile(handle, data + offset, request, &written, nullptr))
        {
            return Result<void>(Failure{ TranslateWin32Error(GetLastError()) });
        }

        if (written == 0)
        {
            return Result<void>(Failure{ Error(ErrorCode::IOError) });
        }

        offset += written;
    }

    return Result<void>();
}

std::atomic<ConsoleSignalCallback> g_signalCallback{ nullptr };

BOOL WINAPI ConsoleControlRoutine(
    DWORD controlType)
{
    ConsoleSignal signal;

    switch (controlType)
    {
    case CTRL_C_EVENT:
        signal = ConsoleSignal::Interrupt;
        break;
    case CTRL_BREAK_EVENT:
        signal = ConsoleSignal::Break;
        break;
    default:
        return FALSE;  // close, logoff, shutdown: default behaviour
    }

    const ConsoleSignalCallback callback = g_signalCallback.load();

    if (callback == nullptr)
    {
        return FALSE;
    }

    return callback(signal) ? TRUE : FALSE;
}

} // namespace

Result<void> WriteStd(
    StdStream stream,
    const char* data,
    Size size) noexcept
{
    fflush(stdout);
    fflush(stderr);

    if (size == 0)
    {
        return Result<void>();
    }

    HANDLE handle = StdHandle(stream);

    if (handle == nullptr || handle == INVALID_HANDLE_VALUE)
    {
        return Result<void>();
    }

    if (IsConsoleHandle(handle))
    {
        return WriteConsoleUtf8(handle, data, size);
    }

    return WriteFileAll(handle, data, size);
}

bool IsTerminal(
    StdStream stream) noexcept
{
    return IsConsoleHandle(StdHandle(stream));
}

Result<void> InstallConsoleSignalHandler(
    ConsoleSignalCallback callback) noexcept
{
    g_signalCallback.store(callback);

    if (!SetConsoleCtrlHandler(ConsoleControlRoutine, TRUE))
    {
        g_signalCallback.store(nullptr);
        return Result<void>(Failure{ TranslateWin32Error(GetLastError()) });
    }

    return Result<void>();
}

void RemoveConsoleSignalHandler() noexcept
{
    SetConsoleCtrlHandler(ConsoleControlRoutine, FALSE);
    g_signalCallback.store(nullptr);
}

void TerminateProcessNow(
    u32 exitCode) noexcept
{
    TerminateProcess(GetCurrentProcess(), exitCode);

    // TerminateProcess on the current process does not return; this is
    // only here so the function honours [[noreturn]] if it somehow does.
    for (;;)
    {
        ExitProcess(exitCode);
    }
}

} // namespace forge::core::platform
