#pragma once

#include <cstdio>
#include <cstdlib>

#if defined(_MSC_VER)
    #include <intrin.h>
#endif

///============================================================================
/// Forge Assertions (Phase 7.9, rule R10)
///
/// FORGE_RELEASE_ASSERT(cond)
///     Always checked, in debug AND release builds. On failure it prints the
///     condition and location to stderr and terminates the process with a
///     fast-fail: __fastfail(FAST_FAIL_FATAL_APP_EXIT) on Windows, which
///     cannot be caught or hijacked by an SEH handler, and __builtin_trap()
///     elsewhere. Use it for bounds checks, size-overflow checks and
///     invariants whose violation would otherwise be memory corruption.
///
/// FORGE_ASSERT(cond)
///     Same as FORGE_RELEASE_ASSERT. Before Phase 7.9 this was a plain
///     assert() that release builds compiled out, which turned every
///     violated invariant (for example Result<T>::Value() on an error) into
///     silent undefined behaviour. Existing call sites are all cheap.
///
/// FORGE_DEBUG_ASSERT(cond)
///     Debug-only (compiled out when NDEBUG is defined). Use it only for
///     checks that are too expensive for release, never for bounds or
///     memory-safety checks.
///
/// This header deliberately does not include <cassert>: on a case-insensitive
/// filesystem, with this directory on the include path, <assert.h> can
/// resolve to this very file and silently define nothing.
///============================================================================

namespace forge::core::detail
{

[[noreturn]] inline void AssertionFailed(
    const char* condition,
    const char* file,
    int line) noexcept
{
    std::fprintf(stderr, "Forge: assertion failed: %s (%s:%d)\n", condition, file, line);
    std::fflush(stderr);

#if defined(_MSC_VER)
    __fastfail(7); // FAST_FAIL_FATAL_APP_EXIT
#else
    __builtin_trap();
#endif
}

} // namespace forge::core::detail

#ifndef FORGE_RELEASE_ASSERT
    #define FORGE_RELEASE_ASSERT(condition)                                        \
        ((condition) ? static_cast<void>(0)                                        \
                     : ::forge::core::detail::AssertionFailed(#condition, __FILE__, __LINE__))
#endif

#ifndef FORGE_ASSERT
    #define FORGE_ASSERT(condition) FORGE_RELEASE_ASSERT(condition)
#endif

#ifndef FORGE_DEBUG_ASSERT
    #if defined(NDEBUG)
        #define FORGE_DEBUG_ASSERT(condition) static_cast<void>(0)
    #else
        #define FORGE_DEBUG_ASSERT(condition) FORGE_RELEASE_ASSERT(condition)
    #endif
#endif
