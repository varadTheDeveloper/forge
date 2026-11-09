#pragma once

#include <cstddef>
#include <cstdint>

#include "Types.h"

///============================================================================
/// Checked size arithmetic (Phase 7.9, rule R10)
///
/// Every allocation-size and capacity computation in forge-core goes
/// through these helpers, so an attacker-influenced size can never wrap
/// around to a small allocation that later writes overflow (the
/// Vector<u64>::Reserve(SIZE_MAX/8 + 3) heap overflow found by study 13).
///
/// Plain comparisons instead of __builtin_*_overflow, because the
/// standalone smoke tests are also built with MSVC's cl.exe, which has no
/// such builtins.
///============================================================================

namespace forge::core
{

/// Largest element count a container of T may hold. Mirrors mfbt's cap of
/// PTRDIFF_MAX bytes, so pointer differences inside one allocation can
/// never overflow either.
template<typename T>
[[nodiscard]] constexpr Size MaxElementCount() noexcept
{
    return static_cast<Size>(PTRDIFF_MAX) / sizeof(T);
}

/// Sets *out = a * b and returns true, or returns false (leaving *out
/// unchanged) if the product does not fit in Size.
[[nodiscard]] constexpr bool CheckedMul(Size a, Size b, Size* out) noexcept
{
    if (a != 0 && b > static_cast<Size>(SIZE_MAX) / a)
    {
        return false;
    }

    *out = a * b;
    return true;
}

/// Sets *out = a + b and returns true, or returns false (leaving *out
/// unchanged) if the sum does not fit in Size.
[[nodiscard]] constexpr bool CheckedAdd(Size a, Size b, Size* out) noexcept
{
    if (b > static_cast<Size>(SIZE_MAX) - a)
    {
        return false;
    }

    *out = a + b;
    return true;
}

/// The next capacity for a doubling growth policy, saturated at `limit`.
/// Returns `current` unchanged when it is already at the limit, which
/// callers must treat as "cannot grow".
[[nodiscard]] constexpr Size GrowCapacity(Size current, Size limit) noexcept
{
    if (current == 0)
    {
        return limit == 0 ? 0 : 1;
    }

    if (current >= limit / 2)
    {
        return limit;
    }

    return current * 2;
}

} // namespace forge::core
