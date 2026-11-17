// Standalone test for runtime/Encoding (Phase 10.1): UTF-8 encode and
// decode with the WHATWG replacement rules, encodeInto, and forgiving
// base64. Portable: runs in the Linux sanitizer job and the Windows MSVC job.
//
// Build (g++/clang, from the repository root):
//   g++ -std=c++17 -fno-exceptions -fsanitize=address,undefined -iquote .
//       runtime/EncodingTest.cpp runtime/Encoding.cpp
//       forge-core/memory/DefaultAllocator.cpp forge-core/memory/detail/AllocationBackend.cpp

#include "runtime/Encoding.h"

#include <cstdio>
#include <cstring>
#include <initializer_list>

using namespace forge::core;
using namespace forge::runtime::encoding;

namespace
{

int g_failures = 0;
int g_checks = 0;

void Check(bool condition, const char* what)
{
    ++g_checks;

    if (!condition)
    {
        std::fprintf(stderr, "  check failed: %s\n", what);
        ++g_failures;
    }
}

Size Length16(const char16_t* s)
{
    Size n = 0;

    while (s[n] != 0)
    {
        ++n;
    }

    return n;
}

LabelLookup Find(const char16_t* label, Size& encoding)
{
    return FindEncoding(label, Length16(label), encoding);
}

template <typename T>
struct NonDeduced
{
    using Type = T;
};

template <typename T>
bool Equals(const Vector<T>& actual, std::initializer_list<typename NonDeduced<T>::Type> expected)
{
    if (actual.Size() != expected.size())
    {
        return false;
    }

    Size i = 0;

    for (T value : expected)
    {
        if (actual[i++] != value)
        {
            return false;
        }
    }

    return true;
}

void ExpectEncode(const char16_t* input, std::initializer_list<u8> expected, const char* what)
{
    Vector<u8> out;
    Check(EncodeUtf8(input, Length16(input), out) && Equals(out, expected), what);
}

void ExpectDecode(std::initializer_list<u8> input, std::initializer_list<char16_t> expected,
                  const char* what, bool final = true, Size expectedConsumed = static_cast<Size>(-1))
{
    Vector<u8> bytes;

    for (u8 b : input)
    {
        bytes.PushBack(b).Ignore();
    }

    Vector<char16_t> out;
    Size consumed = 0;
    const Status status = DecodeUtf8(bytes.Data(), bytes.Size(), false, final, out, consumed);
    const Size wantConsumed = expectedConsumed == static_cast<Size>(-1) ? bytes.Size() : expectedConsumed;
    Check(status == Status::Ok && Equals(out, expected) && consumed == wantConsumed, what);
}

Status DecodeFatal(std::initializer_list<u8> input, bool final, Size& consumed)
{
    Vector<u8> bytes;

    for (u8 b : input)
    {
        bytes.PushBack(b).Ignore();
    }

    Vector<char16_t> out;
    return DecodeUtf8(bytes.Data(), bytes.Size(), true, final, out, consumed);
}

void ExpectAtob(const char16_t* input, const char* expected, const char* what)
{
    Vector<u8> out;
    const Status status = DecodeBase64(input, Length16(input), out);
    const Size n = std::strlen(expected);
    bool ok = status == Status::Ok && out.Size() == n;

    for (Size i = 0; ok && i < n; ++i)
    {
        ok = out[i] == static_cast<u8>(expected[i]);
    }

    Check(ok, what);
}

void ExpectAtobInvalid(const char16_t* input, const char* what)
{
    Vector<u8> out;
    Check(DecodeBase64(input, Length16(input), out) == Status::Invalid, what);
}

void ExpectBtoa(const char16_t* input, const char* expected, const char* what)
{
    Vector<char> out;
    const Status status = EncodeBase64(input, Length16(input), out);
    Check(status == Status::Ok && out.Size() == std::strlen(expected) &&
              (out.Size() == 0 || std::memcmp(out.Data(), expected, out.Size()) == 0),
          what);
}

} // namespace

int main()
{
    // UTF-8 encode.
    ExpectEncode(u"", {}, "encode empty");
    ExpectEncode(u"a", {0x61}, "encode ASCII");
    ExpectEncode(u"\u00A9", {0xC2, 0xA9}, "encode 2 bytes");
    ExpectEncode(u"\u20AC", {0xE2, 0x82, 0xAC}, "encode 3 bytes");
    ExpectEncode(u"\U0001F600", {0xF0, 0x9F, 0x98, 0x80}, "encode a surrogate pair");
    ExpectEncode(u"\xD800", {0xEF, 0xBF, 0xBD}, "lone high surrogate -> U+FFFD");
    ExpectEncode(u"\xDC00", {0xEF, 0xBF, 0xBD}, "lone low surrogate -> U+FFFD");
    ExpectEncode(u"\xD800\x61", {0xEF, 0xBF, 0xBD, 0x61}, "high surrogate then ASCII");
    ExpectEncode(u"\xDC00\xD800", {0xEF, 0xBF, 0xBD, 0xEF, 0xBF, 0xBD}, "reversed pair -> two U+FFFD");

    // encodeInto.
    {
        u8 buffer[8] = {};
        Size read = 0;
        Size written = 0;
        EncodeUtf8Into(u"a\u20ACb", 3, buffer, 2, read, written);
        Check(read == 1 && written == 1 && buffer[0] == 0x61, "encodeInto stops before a code point that does not fit");
        EncodeUtf8Into(u"a\u20ACb", 3, buffer, 4, read, written);
        Check(read == 2 && written == 4 && buffer[1] == 0xE2 && buffer[3] == 0xAC, "encodeInto fills exactly");
        EncodeUtf8Into(u"\U0001F600", 2, buffer, 3, read, written);
        Check(read == 0 && written == 0, "encodeInto never splits a surrogate pair");
        EncodeUtf8Into(u"\U0001F600", 2, buffer, 4, read, written);
        Check(read == 2 && written == 4, "encodeInto counts a pair as two units read");
        EncodeUtf8Into(u"\xD800", 1, buffer, 8, read, written);
        Check(read == 1 && written == 3 && buffer[0] == 0xEF, "encodeInto writes U+FFFD for a lone surrogate");
    }

    // UTF-8 decode, replacement mode (maximal subparts, WHATWG).
    ExpectDecode({}, {}, "decode empty");
    ExpectDecode({0x61, 0x62}, {u'a', u'b'}, "decode ASCII");
    ExpectDecode({0xE2, 0x82, 0xAC}, {0x20AC}, "decode 3 bytes");
    ExpectDecode({0xF0, 0x9F, 0x98, 0x80}, {0xD83D, 0xDE00}, "decode 4 bytes to a pair");
    ExpectDecode({0xEF, 0xBB, 0xBF, 0x61}, {0xFEFF, u'a'}, "a BOM is decoded (TextDecoder strips it)");
    ExpectDecode({0xC2, 0x41}, {0xFFFD, u'A'}, "truncated 2-byte sequence, then ASCII");
    ExpectDecode({0xE0, 0x80}, {0xFFFD, 0xFFFD}, "E0 80: overlong lead, then a lone continuation");
    ExpectDecode({0xF0, 0x90, 0x80}, {0xFFFD}, "incomplete 4-byte sequence at the end: one U+FFFD");
    ExpectDecode({0xF0, 0x90, 0x80, 0x41}, {0xFFFD, u'A'}, "incomplete 4-byte sequence, then ASCII");
    ExpectDecode({0xED, 0xA0, 0x80}, {0xFFFD, 0xFFFD, 0xFFFD}, "an encoded surrogate is three errors");
    ExpectDecode({0xC0, 0x80}, {0xFFFD, 0xFFFD}, "overlong C0 80");
    ExpectDecode({0xF5, 0x80}, {0xFFFD, 0xFFFD}, "F5 is never a lead byte");
    ExpectDecode({0xF4, 0x90, 0x80, 0x80}, {0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD}, "above U+10FFFF");
    ExpectDecode({0xFF, 0xFE}, {0xFFFD, 0xFFFD}, "FF FE");
    ExpectDecode({0x80}, {0xFFFD}, "a lone continuation byte");

    // Stream mode: an incomplete sequence at the end is kept.
    ExpectDecode({0xE2, 0x82}, {}, "stream: incomplete sequence kept", false, 0);
    ExpectDecode({0x41, 0xE2}, {u'A'}, "stream: ASCII decoded, lead byte kept", false, 1);
    ExpectDecode({0x41, 0xE2, 0x82, 0xAC}, {u'A', 0x20AC}, "stream: complete input fully consumed", false, 4);
    ExpectDecode({0xC2, 0x41, 0xE2}, {0xFFFD, u'A'}, "stream: an error is still replaced", false, 2);

    // Fatal mode.
    {
        Size consumed = 0;
        Check(DecodeFatal({0x61, 0x62}, true, consumed) == Status::Ok && consumed == 2, "fatal: valid input");
        Check(DecodeFatal({0xC2}, true, consumed) == Status::Invalid, "fatal: incomplete at the end");
        Check(DecodeFatal({0xC2}, false, consumed) == Status::Ok && consumed == 0, "fatal: incomplete, more to come");
        Check(DecodeFatal({0x80}, true, consumed) == Status::Invalid, "fatal: lone continuation");
        Check(DecodeFatal({0xED, 0xA0, 0x80}, true, consumed) == Status::Invalid, "fatal: encoded surrogate");
        Check(DecodeFatal({0xFF}, false, consumed) == Status::Invalid, "fatal: invalid byte while streaming");
    }

    // Labels (WHATWG "get an encoding").
    {
        Size encoding = 99;
        Check(Find(u"utf-8", encoding) == LabelLookup::Supported && encoding == kUtf8,
              "label utf-8");
        Check(Find(u" \t UNICODE-1-1-UTF-8\n\f\r", encoding) == LabelLookup::Supported &&
                  encoding == kUtf8,
              "label with ASCII whitespace, upper case");
        Check(Find(u"latin1", encoding) == LabelLookup::Supported &&
                  std::strcmp(EncodingName(encoding), "windows-1252") == 0,
              "latin1 is windows-1252");
        Check(Find(u"utf-16", encoding) == LabelLookup::Supported &&
                  std::strcmp(EncodingName(encoding), "utf-16le") == 0,
              "utf-16 is utf-16le");
        Check(Find(u"iso-8859-8-i", encoding) == LabelLookup::Supported &&
                  std::strcmp(EncodingName(encoding), "iso-8859-8-i") == 0,
              "iso-8859-8-i keeps its own name");
        Check(Find(u"x-user-defined", encoding) == LabelLookup::Supported,
              "x-user-defined");
        Check(Find(u"iso-2022-kr", encoding) == LabelLookup::Replacement,
              "replacement labels are refused");
        Check(Find(u"shift_jis", encoding) == LabelLookup::Unsupported, "CJK: unsupported");
        Check(FindEncoding(u"utf-8\0x", 6, encoding) == LabelLookup::Unknown, "a NUL is not whitespace");
        Check(Find(u"\xA0utf-8", encoding) == LabelLookup::Unknown,
              "a non-breaking space is not ASCII whitespace");
        Check(Find(u"utf-\x212A", encoding) == LabelLookup::Unknown,
              "only ASCII letters fold (KELVIN SIGN is not k)");
        Check(Find(u"", encoding) == LabelLookup::Unknown, "empty label");
        Check(Find(u"   ", encoding) == LabelLookup::Unknown, "whitespace only");
        Check(Find(u"utf8 x", encoding) == LabelLookup::Unknown, "inner space");
        Check(EncodingCount() == 32, "32 supported encodings");
    }

    // UTF-16 and single-byte decoding.
    {
        Size le = 0;
        Size be = 0;
        Size w1252 = 0;
        Size iso3 = 0;
        Size user = 0;
        (void)Find(u"utf-16le", le);
        (void)Find(u"utf-16be", be);
        (void)Find(u"windows-1252", w1252);
        (void)Find(u"iso-8859-3", iso3);
        (void)Find(u"x-user-defined", user);

        auto decode = [](Size encoding, std::initializer_list<u8> input, bool fatal, bool final,
                         Vector<char16_t>& out, Size& consumed)
        {
            Vector<u8> bytes;

            for (u8 b : input)
            {
                bytes.PushBack(b).Ignore();
            }

            return Decode(encoding, bytes.Data(), bytes.Size(), fatal, final, out, consumed);
        };

        Vector<char16_t> out;
        Size consumed = 0;
        Check(decode(le, {0x41, 0x00, 0x3D, 0xD8, 0x00, 0xDE}, false, true, out, consumed) == Status::Ok &&
                  Equals(out, {u'A', 0xD83D, 0xDE00}) && consumed == 6,
              "utf-16le with a pair");
        out.Clear();
        Check(decode(be, {0x00, 0x41, 0xD8, 0x3D, 0xDE, 0x00}, false, true, out, consumed) == Status::Ok &&
                  Equals(out, {u'A', 0xD83D, 0xDE00}),
              "utf-16be with a pair");
        out.Clear();
        Check(decode(le, {0x00, 0xDC, 0x41, 0x00}, false, true, out, consumed) == Status::Ok &&
                  Equals(out, {0xFFFD, u'A'}),
              "utf-16: a lone trail surrogate");
        out.Clear();
        Check(decode(le, {0x00, 0xD8, 0x41, 0x00}, false, true, out, consumed) == Status::Ok &&
                  Equals(out, {0xFFFD, u'A'}),
              "utf-16: a lead surrogate, then a unit that is looked at again");
        out.Clear();
        Check(decode(le, {0x00, 0xD8, 0x41}, false, true, out, consumed) == Status::Ok &&
                  Equals(out, {0xFFFD}),
              "utf-16: a lead surrogate and an odd byte at the end are one error");
        out.Clear();
        Check(decode(le, {0x41, 0x00, 0x42}, false, false, out, consumed) == Status::Ok &&
                  Equals(out, {u'A'}) && consumed == 2,
              "utf-16 stream: an odd byte is kept");
        out.Clear();
        Check(decode(le, {0x41, 0x00, 0x3D, 0xD8}, false, false, out, consumed) == Status::Ok &&
                  Equals(out, {u'A'}) && consumed == 2,
              "utf-16 stream: a lead surrogate is kept");
        out.Clear();
        Check(decode(le, {0x41}, true, true, out, consumed) == Status::Invalid,
              "utf-16 fatal: truncated code unit");
        out.Clear();
        Check(decode(w1252, {0x41, 0x80, 0x9F, 0xFF, 0x81}, false, true, out, consumed) == Status::Ok &&
                  Equals(out, {u'A', 0x20AC, 0x0178, 0x00FF, 0x0081}),
              "windows-1252 table");
        out.Clear();
        Check(decode(iso3, {0xA5}, false, true, out, consumed) == Status::Ok && Equals(out, {0xFFFD}),
              "iso-8859-3: an unmapped byte is U+FFFD");
        out.Clear();
        Check(decode(iso3, {0xA5}, true, true, out, consumed) == Status::Invalid,
              "iso-8859-3 fatal: an unmapped byte");
        out.Clear();
        Check(decode(user, {0x00, 0x7F, 0x80, 0xFF}, true, true, out, consumed) == Status::Ok &&
                  Equals(out, {0x0000, 0x007F, 0xF780, 0xF7FF}),
              "x-user-defined");
    }

    // Forgiving base64 (atob).
    ExpectAtob(u"", "", "atob empty");
    ExpectAtob(u"YQ==", "a", "atob with padding");
    ExpectAtob(u"YQ", "a", "atob without padding");
    ExpectAtob(u"YWI=", "ab", "atob two bytes");
    ExpectAtob(u"YWJj", "abc", "atob three bytes");
    ExpectAtob(u" Y\tQ\n=\f=\r", "a", "atob ignores ASCII whitespace");
    ExpectAtob(u"YR", "a", "atob discards the extra bits");
    ExpectAtob(u"/w==", "\xFF", "atob 0xFF");
    ExpectAtobInvalid(u"Y", "atob: 4n+1 characters");
    ExpectAtobInvalid(u"YQ=", "atob: padding only with a multiple of 4");
    ExpectAtobInvalid(u"YQ===", "atob: three padding characters");
    ExpectAtobInvalid(u"====", "atob: padding only");
    ExpectAtobInvalid(u"=", "atob: one padding character");
    ExpectAtobInvalid(u"YQ\u00A0", "atob: non-breaking space is not whitespace here");
    ExpectAtobInvalid(u"Y-Q=", "atob: base64url characters are not accepted");
    ExpectAtobInvalid(u"a=bc", "atob: padding in the middle");

    // btoa.
    ExpectBtoa(u"", "", "btoa empty");
    ExpectBtoa(u"a", "YQ==", "btoa one byte");
    ExpectBtoa(u"ab", "YWI=", "btoa two bytes");
    ExpectBtoa(u"abc", "YWJj", "btoa three bytes");
    ExpectBtoa(u"\u00FF\u00FE", "//4=", "btoa Latin-1");
    {
        Vector<char> out;
        Check(EncodeBase64(u"a\u0100", 2, out) == Status::Invalid, "btoa refuses a unit above 0xFF");
    }

    // Round trip of every byte value.
    {
        char16_t all[256];

        for (int i = 0; i < 256; ++i)
        {
            all[i] = static_cast<char16_t>(i);
        }

        Vector<char> encoded;
        Vector<char16_t> wide;
        Vector<u8> decoded;
        bool ok = EncodeBase64(all, 256, encoded) == Status::Ok;

        for (Size i = 0; ok && i < encoded.Size(); ++i)
        {
            ok = wide.PushBack(static_cast<char16_t>(encoded[i])).HasValue();
        }

        ok = ok && DecodeBase64(wide.Data(), wide.Size(), decoded) == Status::Ok && decoded.Size() == 256;

        for (Size i = 0; ok && i < 256; ++i)
        {
            ok = decoded[i] == i;
        }

        Check(ok, "base64 round trip of every byte");
    }

    if (g_failures != 0)
    {
        std::fprintf(stderr, "EncodingTest: %d of %d checks failed\n", g_failures, g_checks);
        return 1;
    }

    std::printf("EncodingTest: all %d checks passed\n", g_checks);
    return 0;
}
