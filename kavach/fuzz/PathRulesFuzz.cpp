// libFuzzer target for kavach/PathRules (rule R24). Checks invariants that
// must hold for every input, not just "does not crash":
//   * a canonical path canonicalizes to itself (idempotence);
//   * it contains no '/', no "." or ".." component and no empty component;
//   * it is covered by itself and by its parent, and never by a sibling
//     that only shares a prefix.
//
// Build and run (clang, from the repository root):
//   clang++ -std=c++17 -g -O1 -fno-exceptions -fsanitize=fuzzer,address,undefined -iquote .
//       kavach/fuzz/PathRulesFuzz.cpp kavach/PathRules.cpp
//       forge-core/memory/DefaultAllocator.cpp forge-core/memory/detail/AllocationBackend.cpp
//       -o PathRulesFuzz
//   ./PathRulesFuzz -max_total_time=60 kavach/fuzz/corpus

#include "kavach/PathRules.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

using forge::core::Size;
using forge::core::String;
using forge::core::StringView;
using namespace forge::kavach;

static void Require(bool condition)
{
    if (!condition)
    {
        abort();
    }
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    // First byte picks the base folder (byte % 4), so relative paths are
    // covered too. The seed corpus uses the digits 0-3 to stay plain text.
    static const char* const kBases[] = { "C:\\work\\project", "\\\\srv\\share\\dir", "D:\\", "" };

    if (size == 0)
    {
        return 0;
    }

    const char* base = kBases[data[0] % 4];
    const StringView input(reinterpret_cast<const char*>(data + 1), size - 1);

    CanonicalPath out;

    if (LexicalCanonicalize(input, StringView(base), out) != PathError::None)
    {
        Require(out.text.Empty());
        return 0;
    }

    const StringView text = out.text.View();
    Require(!text.Empty());

    // Idempotent.
    CanonicalPath again;
    Require(LexicalCanonicalize(text, StringView(), again) == PathError::None);
    Require(again.text.View() == text);
    Require(again.componentCount == out.componentCount);

    // No forward slashes, and no ".", ".." or empty components after the root.
    for (Size i = 0; i < text.Size(); ++i)
    {
        Require(text.Data()[i] != '/');
    }

    Require(!HasComponent(text, ".") && !HasComponent(text, ".."));

    // Covered by itself and by its parent; the parent is never inside it.
    Require(PathCovers(text, text));
    const StringView parent = ParentPath(text);
    Require(PathCovers(parent, text));
    Require(IsRootPath(text) == (parent.Size() == text.Size()));

    if (!IsRootPath(text))
    {
        Require(!PathCovers(text, parent));
        Require(!LastComponent(text).Empty());

        // "<path>x" is a sibling sharing a prefix, never covered.
        String sibling;
        Require(sibling.Append(text).HasValue() && sibling.Append('x').HasValue());
        Require(!PathCovers(text, sibling.View()));
    }

    return 0;
}
