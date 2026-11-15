// libFuzzer target for runtime/Json (Phase 9.2, rule R24). For every input
// that parses, the document is written back out as JSON and parsed again:
// the second parse must succeed and give the same structure, the same
// member order and the same strings (so the parser never accepts something
// it cannot represent, and never loses or reorders data).
//
// Build and run (clang, from the repository root):
//   clang++ -std=c++17 -g -O1 -fno-exceptions -fsanitize=fuzzer,address,undefined -iquote .
//       runtime/fuzz/JsonFuzz.cpp runtime/Json.cpp
//       forge-core/memory/DefaultAllocator.cpp forge-core/memory/detail/AllocationBackend.cpp
//       -o JsonFuzz
//   ./JsonFuzz -max_total_time=60 runtime/fuzz/json-corpus

#include "runtime/Json.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

using forge::core::Size;
using forge::core::String;
using forge::core::StringView;
using namespace forge::runtime::json;

static void Require(bool condition)
{
    if (!condition)
    {
        abort();
    }
}

static void WriteString(StringView text, String& out)
{
    static const char kHex[] = "0123456789abcdef";
    Require(out.Append('"').HasValue());

    for (Size i = 0; i < text.Size(); ++i)
    {
        const unsigned char c = static_cast<unsigned char>(text.Data()[i]);

        if (c == '"' || c == '\\')
        {
            Require(out.Append('\\').HasValue() && out.Append(static_cast<char>(c)).HasValue());
        }
        else if (c < 0x20)
        {
            Require(out.Append(StringView("\\u00")).HasValue() && out.Append(kHex[c >> 4]).HasValue() &&
                    out.Append(kHex[c & 15]).HasValue());
        }
        else
        {
            Require(out.Append(static_cast<char>(c)).HasValue());
        }
    }

    Require(out.Append('"').HasValue());
}

static void Write(const Document& doc, const Node& node, String& out)
{
    switch (node.kind)
    {
    case Kind::Null: Require(out.Append(StringView("null")).HasValue()); return;
    case Kind::True: Require(out.Append(StringView("true")).HasValue()); return;
    case Kind::False: Require(out.Append(StringView("false")).HasValue()); return;
    case Kind::Number: Require(out.Append(node.text.View()).HasValue()); return;
    case Kind::String: WriteString(node.text.View(), out); return;
    case Kind::Array:
        Require(out.Append('[').HasValue());
        for (Size i = 0; i < node.children.Size(); ++i)
        {
            if (i > 0) Require(out.Append(',').HasValue());
            Write(doc, doc.At(node.children[i]), out);
        }
        Require(out.Append(']').HasValue());
        return;
    case Kind::Object:
        Require(out.Append('{').HasValue());
        for (Size i = 0; i < node.children.Size(); ++i)
        {
            if (i > 0) Require(out.Append(',').HasValue());
            WriteString(node.keys[i].View(), out);
            Require(out.Append(':').HasValue());
            Write(doc, doc.At(node.children[i]), out);
        }
        Require(out.Append('}').HasValue());
        return;
    }
}

static void Same(const Document& a, const Node& x, const Document& b, const Node& y)
{
    Require(x.kind == y.kind);
    Require(x.text.View() == y.text.View());
    Require(x.children.Size() == y.children.Size() && x.keys.Size() == y.keys.Size());

    for (Size i = 0; i < x.keys.Size(); ++i)
    {
        Require(x.keys[i].View() == y.keys[i].View());
    }

    for (Size i = 0; i < x.children.Size(); ++i)
    {
        Same(a, a.At(x.children[i]), b, b.At(y.children[i]));
    }
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    Document first;
    ParseError error;

    if (!Parse(StringView(reinterpret_cast<const char*>(data), size), first, error))
    {
        Require(error.message != nullptr);
        return 0;
    }

    String text;
    Write(first, first.Root(), text);

    Document second;
    Require(Parse(text.View(), second, error));
    Same(first, first.Root(), second, second.Root());
    return 0;
}
