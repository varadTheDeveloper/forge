// libFuzzer target for runtime/Manifest (Phase 9.2, rule R24). The first
// line of the input is a subpath to resolve, the rest is forge.json. For
// every manifest that validates, resolving the subpath (and "." and each
// exports key with its * replaced) must never give a target that leaves
// the package: a Found target starts with "./" and has no "." or ".."
// segment, no empty segment, and no '\', '%', ':' or '*'.
//
// Build and run (clang, from the repository root):
//   clang++ -std=c++17 -g -O1 -fno-exceptions -fsanitize=fuzzer,address,undefined -iquote .
//       runtime/fuzz/ManifestFuzz.cpp runtime/Manifest.cpp runtime/Json.cpp
//       forge-core/memory/DefaultAllocator.cpp forge-core/memory/detail/AllocationBackend.cpp
//       -o ManifestFuzz
//   ./ManifestFuzz -max_total_time=60 runtime/fuzz/manifest-corpus

#include "runtime/Manifest.h"

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

static void CheckTarget(StringView t)
{
    Require(t.Size() > 2 && t.Data()[0] == '.' && t.Data()[1] == '/');
    Size start = 2;

    while (start <= t.Size())
    {
        Size end = start;

        while (end < t.Size() && t.Data()[end] != '/')
        {
            const char c = t.Data()[end];
            Require(c != '\\' && c != '%' && c != ':' && c != '*');
            ++end;
        }

        const StringView segment(t.Data() + start, end - start);
        Require(!segment.Empty() && !(segment == StringView(".")) && !(segment == StringView("..")));

        if (end >= t.Size())
        {
            break;
        }

        start = end + 1;
    }
}

static void Try(const Manifest& m, StringView subpath)
{
    String target;

    if (ResolveExport(m, subpath, target) == ExportResult::Found)
    {
        CheckTarget(target.View());
    }
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    const char* text = reinterpret_cast<const char*>(data);
    Size newline = 0;

    while (newline < size && text[newline] != '\n')
    {
        ++newline;
    }

    const StringView subpath(text, newline);
    const StringView json = newline < size ? StringView(text + newline + 1, size - newline - 1) : StringView();

    Manifest manifest;
    String message;

    if (!ParseManifest(json, false, manifest, message))
    {
        Require(!message.Empty());
        return 0;
    }

    Try(manifest, subpath);
    Try(manifest, StringView("."));

    if (manifest.hasExports)
    {
        const auto& exports = manifest.document.At(manifest.exportsNode);

        for (Size i = 0; i < exports.keys.Size(); ++i)
        {
            String key;

            for (Size k = 0; k < exports.keys[i].Size(); ++k)
            {
                const char c = exports.keys[i].Data()[k];
                Require(c == '*' ? key.Append(StringView("x/../y")).HasValue() : key.Append(c).HasValue());
            }

            Try(manifest, key.View());
            Try(manifest, exports.keys[i].View());
        }
    }

    // Bare specifier splitting never yields an unsafe subpath.
    String name;
    String sub;

    if (SplitBareSpecifier(subpath, name, sub) && !(sub.View() == StringView(".")))
    {
        CheckTarget(sub.View());
    }

    return 0;
}
