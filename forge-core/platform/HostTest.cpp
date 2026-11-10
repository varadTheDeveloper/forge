// Standalone test for Phase 8's Windows host layer: UTF-8/UTF-16 conversion
// (Unicode.h), environment variables (Environment.h) and standard output
// (Stdio.h). Not part of the production moz.build build.
//
// Build (MSVC, from forge-core\):
//   cl /std:c++17 /EHs- /W4 platform\HostTest.cpp platform\Unicode.cpp platform\Environment.cpp
//      platform\Stdio.cpp platform\Win32Error.cpp memory\DefaultAllocator.cpp
//      memory\detail\AllocationBackend.cpp

#include "Environment.h"
#include "Stdio.h"
#include "Unicode.h"

#include <cstdio>
#include <cstring>

#if !defined(WIN32_LEAN_AND_MEAN)
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

using namespace forge::core;
using namespace forge::core::platform;

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

bool Equals(const String& s, const char* expected)
{
    return s.Size() == std::strlen(expected) && std::memcmp(s.Data(), expected, s.Size()) == 0;
}

void Test_Utf8ToUtf16()
{
    std::printf("Test_Utf8ToUtf16...\n");

    Result<Vector<wchar_t>> empty = Utf8ToUtf16(StringView(""));
    CHECK(empty.HasValue() && empty.Value().Size() == 1 && empty.Value()[0] == L'\0');

    // "héllo \U0001F525"
    Result<Vector<wchar_t>> text = Utf8ToUtf16(StringView("h\xc3\xa9llo \xf0\x9f\x94\xa5"));
    CHECK(text.HasValue());

    if (text.HasValue())
    {
        const Vector<wchar_t>& w = text.Value();
        CHECK(w.Size() == 9);  // 6 + surrogate pair + terminator
        CHECK(w[1] == 0x00E9);
        CHECK(w[6] == 0xD83D && w[7] == 0xDD25);
        CHECK(w[8] == L'\0');
    }

    // Invalid UTF-8 becomes U+FFFD instead of failing.
    Result<Vector<wchar_t>> invalid = Utf8ToUtf16(StringView("a\xff" "b"));
    CHECK(invalid.HasValue() && invalid.Value()[1] == 0xFFFD);
}

void Test_Utf16ToUtf8()
{
    std::printf("Test_Utf16ToUtf8...\n");

    const wchar_t text[] = { L'h', 0x00E9, 0xD83D, 0xDD25 };
    String out;
    CHECK(AppendUtf16AsUtf8(text, 4, out).HasValue());
    CHECK(Equals(out, "h\xc3\xa9\xf0\x9f\x94\xa5"));

    // An unpaired surrogate becomes U+FFFD.
    const wchar_t lone[] = { L'a', 0xD800, L'b' };
    String loneOut;
    CHECK(AppendUtf16AsUtf8(lone, 3, loneOut).HasValue());
    CHECK(Equals(loneOut, "a\xef\xbf\xbd" "b"));

    // A surrogate pair across the internal 2048-unit piece boundary must
    // survive intact.
    Vector<wchar_t> longText;
    for (int i = 0; i < 2047; ++i)
    {
        CHECK(longText.PushBack(L'x').HasValue());
    }
    CHECK(longText.PushBack(0xD83D).HasValue());
    CHECK(longText.PushBack(0xDD25).HasValue());
    CHECK(longText.PushBack(L'y').HasValue());

    String longOut;
    CHECK(AppendUtf16AsUtf8(longText.Data(), longText.Size(), longOut).HasValue());
    CHECK(longOut.Size() == 2047 + 4 + 1);
    CHECK(std::memcmp(longOut.Data() + 2047, "\xf0\x9f\x94\xa5" "y", 5) == 0);
}

void Test_EnvNames()
{
    std::printf("Test_EnvNames...\n");

    CHECK(IsValidEnvName(StringView("PATH")));
    CHECK(IsValidEnvName(StringView("my.var-1")));
    CHECK(!IsValidEnvName(StringView("")));
    CHECK(!IsValidEnvName(StringView("A=B")));
    CHECK(!IsValidEnvName(StringView("A\0B", 3)));
}

void Test_GetEnv()
{
    std::printf("Test_GetEnv...\n");

    // "värde"
    const wchar_t valueW[] = { L'v', 0x00E4, L'r', L'd', L'e', L'\0' };
    CHECK(SetEnvironmentVariableW(L"FORGE_HOST_TEST_VAR", valueW));
    CHECK(SetEnvironmentVariableW(L"FORGE_HOST_TEST_EMPTY", L""));
    SetEnvironmentVariableW(L"FORGE_HOST_TEST_UNSET", nullptr);

    String value;
    Result<bool> found = GetEnv(StringView("FORGE_HOST_TEST_VAR"), value);
    CHECK(found.HasValue() && found.Value());
    CHECK(Equals(value, "v\xc3\xa4rde"));

    // Names are case-insensitive on Windows.
    String lower;
    Result<bool> foundLower = GetEnv(StringView("forge_host_test_var"), lower);
    CHECK(foundLower.HasValue() && foundLower.Value() && Equals(lower, "v\xc3\xa4rde"));

    String empty;
    Result<bool> foundEmpty = GetEnv(StringView("FORGE_HOST_TEST_EMPTY"), empty);
    CHECK(foundEmpty.HasValue() && foundEmpty.Value() && empty.Size() == 0);

    String unset;
    Result<bool> notFound = GetEnv(StringView("FORGE_HOST_TEST_UNSET"), unset);
    CHECK(notFound.HasValue() && !notFound.Value());

    String bad;
    Result<bool> invalid = GetEnv(StringView("A=B"), bad);
    CHECK(invalid.HasError() && invalid.Error().Code() == ErrorCode::InvalidArgument);

    // A value longer than the first 256-unit buffer.
    Vector<wchar_t> longValue;
    for (int i = 0; i < 1000; ++i)
    {
        CHECK(longValue.PushBack(L'z').HasValue());
    }
    CHECK(longValue.PushBack(L'\0').HasValue());
    CHECK(SetEnvironmentVariableW(L"FORGE_HOST_TEST_LONG", longValue.Data()));

    String longOut;
    Result<bool> foundLong = GetEnv(StringView("FORGE_HOST_TEST_LONG"), longOut);
    CHECK(foundLong.HasValue() && foundLong.Value() && longOut.Size() == 1000);
}

void Test_ListEnv()
{
    std::printf("Test_ListEnv...\n");

    Result<Vector<EnvEntry>> entries = ListEnv();
    CHECK(entries.HasValue());

    if (!entries.HasValue())
    {
        return;
    }

    bool sawTestVar = false;

    for (Size i = 0; i < entries.Value().Size(); ++i)
    {
        const EnvEntry& entry = entries.Value()[i];
        CHECK(entry.name.Size() > 0 && entry.name.Data()[0] != '=');

        if (Equals(entry.name, "FORGE_HOST_TEST_VAR"))
        {
            sawTestVar = Equals(entry.value, "v\xc3\xa4rde");
        }
    }

    CHECK(sawTestVar);
}

void Test_WriteStd()
{
    std::printf("Test_WriteStd...\n");

    const char text[] = "[host-test] UTF-8 line: caf\xc3\xa9 \xe2\x9c\x93\n";
    CHECK(WriteStd(StdStream::Out, text, sizeof(text) - 1).HasValue());
    CHECK(WriteStd(StdStream::Err, "", 0).HasValue());
}

} // namespace

int main()
{
    Test_Utf8ToUtf16();
    Test_Utf16ToUtf8();
    Test_EnvNames();
    Test_GetEnv();
    Test_ListEnv();
    Test_WriteStd();

    if (g_failures != 0)
    {
        std::printf("HostTest: %d failure(s)\n", g_failures);
        return 1;
    }

    std::printf("HostTest: all passed\n");
    return 0;
}
