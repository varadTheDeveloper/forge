// libFuzzer target for runtime/ModuleSpecifier (Phase 9.1, rule R24).
// Invariants checked for every input that resolves:
//   * the result is a normalized file URL: "file:///X:" with an upper-case
//     drive letter, no "." or ".." or empty segments, no '?', '#' or '\';
//   * resolving the result again (as an absolute specifier) gives itself;
//   * it converts to a Windows path and back to the same URL.
//
// Build and run (clang, from the repository root):
//   clang++ -std=c++17 -g -O1 -fno-exceptions -fsanitize=fuzzer,address,undefined -iquote .
//       runtime/fuzz/ModuleSpecifierFuzz.cpp runtime/ModuleSpecifier.cpp
//       forge-core/memory/DefaultAllocator.cpp forge-core/memory/detail/AllocationBackend.cpp
//       -o ModuleSpecifierFuzz
//   ./ModuleSpecifierFuzz -max_total_time=60 runtime/fuzz/corpus

#include "runtime/ModuleSpecifier.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

using forge::core::Size;
using forge::core::String;
using forge::core::StringView;
using namespace forge::runtime;

static void Require(bool condition)
{
    if (!condition)
    {
        abort();
    }
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    // First byte picks the importing module (byte % 3); the seed corpus uses
    // the digits 0-2 to stay plain text.
    static const char* const kBases[] = { "file:///C:/app/src/main.js", "file:///D:/x.js", "" };

    if (size == 0)
    {
        return 0;
    }

    const StringView base(kBases[data[0] % 3]);
    const StringView specifier(reinterpret_cast<const char*>(data + 1), size - 1);

    String url;

    if (ResolveSpecifier(specifier, base, url) != SpecifierError::None)
    {
        return 0;
    }

    const StringView u = url.View();
    Require(u.Size() >= 10);
    Require(StringView(u.Data(), 8) == StringView("file:///"));
    Require(u.Data()[8] >= 'A' && u.Data()[8] <= 'Z' && u.Data()[9] == ':');

    for (Size i = 0; i < u.Size(); ++i)
    {
        const char c = u.Data()[i];
        Require(c != '?' && c != '#' && c != '\\' && c > 0x20 && c < 0x7F);
    }

    // Segments after the drive: none empty (except a bare drive's trailing
    // '/'), none "." or "..".
    Size start = 11;

    while (start < u.Size())
    {
        Size end = start;

        while (end < u.Size() && u.Data()[end] != '/')
        {
            ++end;
        }

        const StringView segment(u.Data() + start, end - start);
        Require(!segment.Empty());
        Require(!(segment == StringView(".")) && !(segment == StringView("..")));
        start = end + 1;
    }

    String again;
    Require(ResolveSpecifier(u, base, again) == SpecifierError::None);
    Require(again.View() == u);

    String path;
    Require(FileUrlToPath(u, path) == SpecifierError::None);

    String back;
    Require(PathToFileUrl(path.View(), back));
    Require(back.View() == u);
    return 0;
}
