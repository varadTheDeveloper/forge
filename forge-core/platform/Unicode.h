#pragma once

// UTF-8 <-> UTF-16 conversion for Win32 calls (Phase 8).
//
// Forge keeps every string in UTF-8. Win32 "W" functions take UTF-16.
// Invalid input is never rejected: an invalid UTF-8 sequence or an unpaired
// UTF-16 surrogate becomes U+FFFD, which is what Windows itself does when
// the strict flags are not passed. Callers that need exact round-trips
// (file names with unpaired surrogates) cannot get them from these
// functions; see HISTORY.md, "Phase 8".

#if !defined(_WIN32)
#error "forge::core::platform::Unicode: Windows-only."
#endif

#include "../Result.h"
#include "../String.h"
#include "../StringView.h"
#include "../Types.h"
#include "../memory/Vector.h"

namespace forge::core::platform
{

/// UTF-8 -> UTF-16, with a terminating L'\0' (not counted by the
/// conversion, but present in the returned vector). Fails with
/// InvalidArgument for input above INT_MAX bytes, or OutOfMemory.
[[nodiscard]]
Result<Vector<wchar_t>> Utf8ToUtf16(
    StringView text) noexcept;

/// UTF-16 -> UTF-8, appended to `out`. `length` is in UTF-16 code units.
[[nodiscard]]
Result<void> AppendUtf16AsUtf8(
    const wchar_t* text,
    Size length,
    String& out) noexcept;

} // namespace forge::core::platform
