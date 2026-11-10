#pragma once

// Environment variables (Phase 8), read through the UTF-16 Win32 API and
// returned as UTF-8. Names are case-insensitive on Windows. Access control
// is not done here: Kavach checks every name before these are called.

#if !defined(_WIN32)
#error "forge::core::platform::Environment: Windows-only."
#endif

#include "../Result.h"
#include "../String.h"
#include "../StringView.h"
#include "../memory/Vector.h"

namespace forge::core::platform
{

/// True if `name` can name an environment variable: not empty, no '=' and
/// no NUL character.
[[nodiscard]]
bool IsValidEnvName(
    StringView name) noexcept;

/// Reads one variable. On success returns true and sets `value`, or
/// returns false (and leaves `value` empty) if the variable is not set.
/// Fails with InvalidArgument if the name is not valid.
[[nodiscard]]
Result<bool> GetEnv(
    StringView name,
    String& value) noexcept;

struct EnvEntry
{
    String name;
    String value;
};

/// Every variable of the process, in the order Windows keeps them. The
/// hidden per-drive entries (names starting with '=') are left out.
[[nodiscard]]
Result<Vector<EnvEntry>> ListEnv() noexcept;

} // namespace forge::core::platform
