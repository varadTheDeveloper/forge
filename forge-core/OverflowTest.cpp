// Standalone regression test for Phase 7.9's checked size arithmetic
// (rule R10). Same role as the other *Test.cpp harnesses: not part of the
// production moz.build build.
//
// Before Phase 7.9, Vector<u64>::Reserve(SIZE_MAX / 8 + 3) computed
// capacity * sizeof(T) without a check, allocated 16 bytes, recorded the
// huge capacity, and the third PushBack wrote past the buffer (ASan:
// heap-buffer-overflow, study 13). String::Append, Queue and HashMap had
// the same pattern. Every case below must now fail with
// ErrorCode::Overflow instead of allocating.
//
// Build (MSVC, from forge-core\):
//   cl /std:c++17 /EHs- /W4 OverflowTest.cpp memory\DefaultAllocator.cpp memory\detail\AllocationBackend.cpp
// Build (g++/clang, sanitizers):
//   g++ -std=c++17 -fno-exceptions -fsanitize=address,undefined OverflowTest.cpp memory/DefaultAllocator.cpp memory/detail/AllocationBackend.cpp

#include "CheckedMath.h"
#include "HashMap.h"
#include "Queue.h"
#include "String.h"
#include "memory/Vector.h"

#include <cstdint>
#include <cstdio>

using namespace forge::core;

namespace
{

int g_failures = 0;

#define CHECK(cond)                                                          \
    do                                                                       \
    {                                                                        \
        if (!(cond))                                                        \
        {                                                                    \
            std::fprintf(stderr, "CHECK FAILED at %s:%d: %s\n", __FILE__,    \
                          __LINE__, #cond);                                  \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

void Test_CheckedMath()
{
    std::printf("Test_CheckedMath...\n");

    Size out = 7;
    CHECK(CheckedMul(3, 4, &out) && out == 12);
    CHECK(!CheckedMul(SIZE_MAX / 2 + 1, 2, &out));
    CHECK(out == 12); // unchanged on failure
    CHECK(CheckedMul(0, SIZE_MAX, &out) && out == 0);
    CHECK(CheckedAdd(SIZE_MAX - 1, 1, &out) && out == SIZE_MAX);
    CHECK(!CheckedAdd(SIZE_MAX, 1, &out));

    CHECK(GrowCapacity(0, 100) == 1);
    CHECK(GrowCapacity(10, 100) == 20);
    CHECK(GrowCapacity(60, 100) == 100);
    CHECK(GrowCapacity(100, 100) == 100); // saturated: cannot grow
    CHECK(MaxElementCount<std::uint64_t>() == static_cast<Size>(PTRDIFF_MAX) / 8);
}

void Test_VectorReserveOverflow()
{
    std::printf("Test_VectorReserveOverflow...\n");

    Vector<std::uint64_t> v;
    Result<void> r = v.Reserve(SIZE_MAX / 8 + 3); // the study-13 repro
    CHECK(r.HasError());
    CHECK(r.HasError() && r.Error().Code() == ErrorCode::Overflow);
    CHECK(v.Capacity() == 0);

    // Still usable afterwards.
    CHECK(v.PushBack(1).HasValue());
    CHECK(v.PushBack(2).HasValue());
    CHECK(v.PushBack(3).HasValue());
    CHECK(v.Size() == 3 && v[2] == 3);

    Vector<std::uint64_t> w;
    CHECK(w.Reserve(MaxElementCount<std::uint64_t>() + 1).HasError());
}

void Test_StringAppendOverflow()
{
    std::printf("Test_StringAppendOverflow...\n");

    String s;
    CHECK(s.Append(StringView("abc")).HasValue());

    Result<void> r = s.Reserve(SIZE_MAX);
    CHECK(r.HasError() && r.Error().Code() == ErrorCode::Overflow);
    CHECK(s.View() == StringView("abc"));

    // A view whose claimed size would wrap size_ + text.Size(). Never
    // dereferenced: Append must reject it before copying anything.
    StringView huge(s.Data(), SIZE_MAX - 1);
    Result<void> r2 = s.Append(huge);
    CHECK(r2.HasError() && r2.Error().Code() == ErrorCode::Overflow);
    CHECK(s.View() == StringView("abc"));
}

void Test_QueueReserveOverflow()
{
    std::printf("Test_QueueReserveOverflow...\n");

    Queue<std::uint64_t> q;
    Result<void> r = q.Reserve(SIZE_MAX / 8 + 3);
    CHECK(r.HasError() && r.Error().Code() == ErrorCode::Overflow);
    CHECK(q.Push(5).HasValue());
    CHECK(q.Size() == 1 && q.Front() == 5);
}

void Test_HashMapReserveOverflow()
{
    std::printf("Test_HashMapReserveOverflow...\n");

    HashMap<std::uint64_t, std::uint64_t> m;
    Result<void> r = m.Reserve(SIZE_MAX / 2);
    CHECK(r.HasError() && r.Error().Code() == ErrorCode::Overflow);

    // Fits the size math but not memory: must fail cleanly (OutOfMemory),
    // never wrap. Under ASan run with ASAN_OPTIONS=allocator_may_return_null=1.
    Result<void> r2 = m.Reserve(SIZE_MAX / 64);
    CHECK(r2.HasError());

    CHECK(m.Insert(1, 2).HasValue());
    CHECK(m.Size() == 1);
}

} // namespace

int main()
{
    Test_CheckedMath();
    Test_VectorReserveOverflow();
    Test_StringAppendOverflow();
    Test_QueueReserveOverflow();
    Test_HashMapReserveOverflow();

    if (g_failures != 0)
    {
        std::printf("%d CHECK(S) FAILED\n", g_failures);
        return 1;
    }

    std::printf("ALL TESTS PASSED\n");
    return 0;
}
