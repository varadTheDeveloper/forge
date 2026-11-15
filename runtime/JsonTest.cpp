// Standalone test for runtime/Json (Phase 9.2). Portable: runs in the Linux
// sanitizer job and the Windows MSVC job.
//
// Build (g++/clang, from the repository root):
//   g++ -std=c++17 -fno-exceptions -fsanitize=address,undefined -iquote .
//       runtime/JsonTest.cpp runtime/Json.cpp
//       forge-core/memory/DefaultAllocator.cpp forge-core/memory/detail/AllocationBackend.cpp

#include "runtime/Json.h"

#include <cstdio>
#include <cstring>

using namespace forge::core;
using namespace forge::runtime::json;

namespace
{

int g_failures = 0;
int g_checks = 0;

bool ParseText(const char* text, Size length, Document& doc, ParseError& error)
{
    return Parse(StringView(text, length), doc, error);
}

void ExpectOk(const char* text)
{
    ++g_checks;
    Document doc;
    ParseError error;

    if (!ParseText(text, std::strlen(text), doc, error))
    {
        std::fprintf(stderr, "  should parse: %s (error: %s at %zu)\n", text, error.message,
                     static_cast<size_t>(error.offset));
        ++g_failures;
    }
}

void ExpectFail(const char* label, const char* text, Size length, const char* messagePart)
{
    ++g_checks;
    Document doc;
    ParseError error;

    if (ParseText(text, length, doc, error))
    {
        std::fprintf(stderr, "  should fail: %s\n", label);
        ++g_failures;
        return;
    }

    if (messagePart != nullptr && std::strstr(error.message, messagePart) == nullptr)
    {
        std::fprintf(stderr, "  %s: message '%s' lacks '%s'\n", label, error.message, messagePart);
        ++g_failures;
    }
}

void ExpectFail(const char* text, const char* messagePart)
{
    ExpectFail(text, text, std::strlen(text), messagePart);
}

void Check(bool condition, const char* what)
{
    ++g_checks;

    if (!condition)
    {
        std::fprintf(stderr, "  check failed: %s\n", what);
        ++g_failures;
    }
}

} // namespace

int main()
{
    ExpectOk("{}");
    ExpectOk("[]");
    ExpectOk("null");
    ExpectOk("  true ");
    ExpectOk("-0.5e+10");
    ExpectOk("0");
    ExpectOk("\"\\u00e9\\ud83d\\udd25\\n\\t\\\"\\\\\\/\"");
    ExpectOk("{\"a\":[1,2,{\"b\":null}],\"c\":\"d\"}");
    ExpectOk("\xEF\xBB\xBF{\"bom\":true}");
    ExpectOk("\"caf\xC3\xA9 \xF0\x9F\x94\xA5\"");

    ExpectFail("", "end of the file");
    ExpectFail("{,}", "quoted key");
    ExpectFail("{\"a\":1,}", "trailing comma");
    ExpectFail("[1,]", "trailing comma");
    ExpectFail("{\"a\":1,\"a\":2}", "twice");
    ExpectFail("{'a':1}", "quoted key");
    ExpectFail("// c\n{}", "JSON value");
    ExpectFail("{} x", "after the end");
    ExpectFail("01", "leading zero");
    ExpectFail("+1", "JSON value");
    ExpectFail("1.", "decimal point");
    ExpectFail("1e", "exponent");
    ExpectFail("NaN", "JSON value");
    ExpectFail("tru", "JSON value");
    ExpectFail("\"abc", "not closed");
    ExpectFail("\"a\\x\"", "unknown escape");
    ExpectFail("\"\\u12\"", "four hexadecimal");
    ExpectFail("\"\\ud800\"", "lone surrogate");
    ExpectFail("\"\\udc00\"", "lone surrogate");
    ExpectFail("\"\\ud800\\u0041\"", "lone surrogate");
    ExpectFail("\"tab\tinside\"", "control character");
    ExpectFail("bad utf8", "\"\xC3\x28\"", 4, "UTF-8");
    ExpectFail("overlong", "\"\xC0\xAF\"", 4, "UTF-8");
    ExpectFail("surrogate utf8", "\"\xED\xA0\x80\"", 5, "UTF-8");
    ExpectFail("beyond U+10FFFF", "\"\xF4\x90\x80\x80\"", 6, "UTF-8");
    ExpectFail("truncated utf8", "\"\xE2\x82\"", 4, "UTF-8");
    ExpectFail("nul byte", "{\"a\":\0}", 7, "JSON value");
    ExpectFail("{\"a\" 1}", "':'");
    ExpectFail("[1 2]", "','");

    // Depth limit.
    {
        char deep[200];
        for (int i = 0; i < 70; ++i) deep[i] = '[';
        for (int i = 0; i < 70; ++i) deep[70 + i] = ']';
        deep[140] = 0;
        ExpectFail("too deep", deep, 140, "64 levels");

        for (int i = 0; i < 60; ++i) deep[i] = '[';
        for (int i = 0; i < 60; ++i) deep[60 + i] = ']';
        deep[120] = 0;
        ExpectOk(deep);
    }

    // Structure and member order.
    {
        Document doc;
        ParseError error;
        const char* text = "{\"z\":\"last\",\"a\":[true,false,null,12.5],\"m\":{\"k\":\"v\"}}";
        const bool ok = ParseText(text, std::strlen(text), doc, error);
        Check(ok, "structured document parses");

        if (ok)
        {
            const Node& root = doc.Root();
            Check(root.kind == Kind::Object && root.keys.Size() == 3, "three members");
            Check(root.keys[0].View() == StringView("z") && root.keys[2].View() == StringView("m"),
                  "member order kept");
            const Node* a = doc.Member(root, StringView("a"));
            Check(a != nullptr && a->kind == Kind::Array && a->children.Size() == 4, "array member");
            Check(a != nullptr && doc.At(a->children[3]).text.View() == StringView("12.5"),
                  "number text kept");
            const Node* m = doc.Member(root, StringView("m"));
            Check(m != nullptr && doc.Member(*m, StringView("k")) != nullptr, "nested member");
            Check(doc.Member(root, StringView("missing")) == nullptr, "missing member");
        }
    }

    {
        Document doc;
        ParseError error;
        const char* text = "\"\\u00e9\\ud83d\\udd25\"";
        Check(ParseText(text, std::strlen(text), doc, error) &&
                  doc.Root().text.View() == StringView("\xC3\xA9\xF0\x9F\x94\xA5"),
              "escapes decode to UTF-8");
    }

    if (g_failures != 0)
    {
        std::fprintf(stderr, "JsonTest: %d of %d checks FAILED\n", g_failures, g_checks);
        return 1;
    }

    std::printf("JsonTest: all %d checks passed\n", g_checks);
    return 0;
}
