// Text and base64 encodings (Phase 10.1). See Encoding.h.

#include "runtime/Encoding.h"

namespace forge::runtime::encoding
{

using forge::core::Size;
using forge::core::u8;
using forge::core::Vector;

namespace
{

constexpr char16_t kReplacement = 0xFFFD;

bool IsHighSurrogate(char16_t unit) noexcept
{
    return unit >= 0xD800 && unit <= 0xDBFF;
}

bool IsLowSurrogate(char16_t unit) noexcept
{
    return unit >= 0xDC00 && unit <= 0xDFFF;
}

/// The code point starting at units[i], and how many units it takes. A lone
/// surrogate is U+FFFD (one unit).
char32_t CodePointAt(const char16_t* units, Size count, Size i, Size& length) noexcept
{
    const char16_t unit = units[i];
    length = 1;

    if (IsHighSurrogate(unit) && i + 1 < count && IsLowSurrogate(units[i + 1]))
    {
        length = 2;
        return 0x10000 + ((static_cast<char32_t>(unit) - 0xD800) << 10) +
               (static_cast<char32_t>(units[i + 1]) - 0xDC00);
    }

    if (IsHighSurrogate(unit) || IsLowSurrogate(unit))
    {
        return kReplacement;
    }

    return unit;
}

Size Utf8Length(char32_t cp) noexcept
{
    return cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
}

/// Writes `cp` as UTF-8 into `out` (which has room for Utf8Length(cp)).
void WriteUtf8(char32_t cp, u8* out) noexcept
{
    if (cp < 0x80)
    {
        out[0] = static_cast<u8>(cp);
    }
    else if (cp < 0x800)
    {
        out[0] = static_cast<u8>(0xC0 | (cp >> 6));
        out[1] = static_cast<u8>(0x80 | (cp & 0x3F));
    }
    else if (cp < 0x10000)
    {
        out[0] = static_cast<u8>(0xE0 | (cp >> 12));
        out[1] = static_cast<u8>(0x80 | ((cp >> 6) & 0x3F));
        out[2] = static_cast<u8>(0x80 | (cp & 0x3F));
    }
    else
    {
        out[0] = static_cast<u8>(0xF0 | (cp >> 18));
        out[1] = static_cast<u8>(0x80 | ((cp >> 12) & 0x3F));
        out[2] = static_cast<u8>(0x80 | ((cp >> 6) & 0x3F));
        out[3] = static_cast<u8>(0x80 | (cp & 0x3F));
    }
}

bool AppendCodePoint(char32_t cp, Vector<char16_t>& out) noexcept
{
    if (cp < 0x10000)
    {
        return out.PushBack(static_cast<char16_t>(cp)).HasValue();
    }

    cp -= 0x10000;
    return out.PushBack(static_cast<char16_t>(0xD800 + (cp >> 10))).HasValue() &&
           out.PushBack(static_cast<char16_t>(0xDC00 + (cp & 0x3FF))).HasValue();
}

/// The value of a base64 alphabet character, or -1.
int Base64Value(char16_t c) noexcept
{
    if (c >= 'A' && c <= 'Z')
    {
        return c - 'A';
    }

    if (c >= 'a' && c <= 'z')
    {
        return c - 'a' + 26;
    }

    if (c >= '0' && c <= '9')
    {
        return c - '0' + 52;
    }

    if (c == '+')
    {
        return 62;
    }

    return c == '/' ? 63 : -1;
}

bool IsAsciiWhitespace(char16_t c) noexcept
{
    return c == 0x09 || c == 0x0A || c == 0x0C || c == 0x0D || c == 0x20;
}

enum class Kind : u8
{
    Utf8,
    SingleByte,
    Utf16Be,
    Utf16Le,
    XUserDefined,
};

struct EncodingRecord
{
    const char* name;
    Kind kind;
    int table;  // index into kSingleByteTables, or -1
};

struct LabelRecord
{
    const char* label;
    u8 encoding;  // index into kEncodings, kLabelReplacement or kLabelUnsupported
};

#include "runtime/EncodingTables.inc"

/// -1, 0 or 1 as `a` (already lower case, at most 64 units) sorts before,
/// equal to or after the ASCII label `b`.
int CompareLabel(const char16_t* a, Size length, const char* b) noexcept
{
    for (Size i = 0;; ++i)
    {
        // A NUL inside the label must not look like its end.
        const unsigned left = i < length ? (a[i] == 0 ? 0x10000u : a[i]) : 0;
        const unsigned right = static_cast<unsigned char>(b[i]);

        if (left != right || left == 0)
        {
            return left < right ? -1 : left > right ? 1 : 0;
        }
    }
}

/// The shared UTF-16 decoder (WHATWG), on whole code units.
Status DecodeUtf16(const u8* bytes, Size count, bool bigEndian, bool fatal, bool final,
                   Vector<char16_t>& out, Size& consumed) noexcept
{
    consumed = 0;

    if (out.Reserve(out.Size() + count / 2 + 1).HasError())
    {
        return Status::OutOfMemory;
    }

    auto error = [&]() -> Status
    {
        if (fatal)
        {
            return Status::Invalid;
        }

        return out.PushBack(kReplacement).HasValue() ? Status::Ok : Status::OutOfMemory;
    };

    Size i = 0;
    const Size whole = count & ~static_cast<Size>(1);

    while (i < whole)
    {
        const char16_t unit = bigEndian ? static_cast<char16_t>((bytes[i] << 8) | bytes[i + 1])
                                        : static_cast<char16_t>((bytes[i + 1] << 8) | bytes[i]);

        if (IsHighSurrogate(unit))
        {
            if (i + 2 >= whole)
            {
                // The pair may continue in the next chunk.
                break;
            }

            const char16_t next = bigEndian
                                      ? static_cast<char16_t>((bytes[i + 2] << 8) | bytes[i + 3])
                                      : static_cast<char16_t>((bytes[i + 3] << 8) | bytes[i + 2]);

            if (IsLowSurrogate(next))
            {
                if (out.PushBack(unit).HasError() || out.PushBack(next).HasError())
                {
                    return Status::OutOfMemory;
                }

                i += 4;
                continue;
            }

            // An unpaired lead surrogate; the next unit is looked at again.
            if (Status s = error(); s != Status::Ok)
            {
                return s;
            }

            i += 2;
            continue;
        }

        if (IsLowSurrogate(unit))
        {
            if (Status s = error(); s != Status::Ok)
            {
                return s;
            }
        }
        else if (out.PushBack(unit).HasError())
        {
            return Status::OutOfMemory;
        }

        i += 2;
    }

    if (i < count)
    {
        // Left: an odd byte, a lead surrogate, or a lead surrogate and an
        // odd byte. At the end of the input they are one error together.
        if (!final)
        {
            consumed = i;
            return Status::Ok;
        }

        if (Status s = error(); s != Status::Ok)
        {
            return s;
        }
    }

    consumed = count;
    return Status::Ok;
}

Status DecodeSingleByte(const u8* bytes, Size count, Kind kind, int table, bool fatal,
                        Vector<char16_t>& out, Size& consumed) noexcept
{
    consumed = 0;

    if (out.Reserve(out.Size() + count).HasError())
    {
        return Status::OutOfMemory;
    }

    for (Size i = 0; i < count; ++i)
    {
        const u8 b = bytes[i];
        char16_t unit = b;

        if (b >= 0x80)
        {
            unit = kind == Kind::XUserDefined ? static_cast<char16_t>(0xF780 + b - 0x80)
                                              : kSingleByteTables[table][b - 0x80];
        }

        if (unit == 0 && b != 0)
        {
            if (fatal)
            {
                return Status::Invalid;
            }

            unit = kReplacement;
        }

        if (out.PushBack(unit).HasError())
        {
            return Status::OutOfMemory;
        }
    }

    consumed = count;
    return Status::Ok;
}

} // namespace

LabelLookup FindEncoding(const char16_t* label, Size length, Size& encoding) noexcept
{
    encoding = 0;
    Size start = 0;

    while (start < length && IsAsciiWhitespace(label[start]))
    {
        ++start;
    }

    while (length > start && IsAsciiWhitespace(label[length - 1]))
    {
        --length;
    }

    // The longest label is 19 characters.
    char16_t lower[64];
    const Size n = length - start;

    if (n == 0 || n >= 64)
    {
        return LabelLookup::Unknown;
    }

    for (Size i = 0; i < n; ++i)
    {
        const char16_t c = label[start + i];
        lower[i] = c >= 'A' && c <= 'Z' ? static_cast<char16_t>(c + 32) : c;
    }

    Size low = 0;
    Size high = sizeof(kLabels) / sizeof(kLabels[0]);

    while (low < high)
    {
        const Size mid = low + (high - low) / 2;
        const int order = CompareLabel(lower, n, kLabels[mid].label);

        if (order == 0)
        {
            const u8 id = kLabels[mid].encoding;

            if (id == kLabelReplacement)
            {
                return LabelLookup::Replacement;
            }

            if (id == kLabelUnsupported)
            {
                return LabelLookup::Unsupported;
            }

            encoding = id;
            return LabelLookup::Supported;
        }

        if (order < 0)
        {
            high = mid;
        }
        else
        {
            low = mid + 1;
        }
    }

    return LabelLookup::Unknown;
}

Size EncodingCount() noexcept
{
    return kEncodingCount;
}

const char* EncodingName(Size encoding) noexcept
{
    return encoding < kEncodingCount ? kEncodings[encoding].name : "";
}

Status Decode(Size encoding, const u8* bytes, Size count, bool fatal, bool final,
              Vector<char16_t>& out, Size& consumed) noexcept
{
    static_assert(sizeof(kEncodings) / sizeof(kEncodings[0]) == kEncodingCount, "table size");
    consumed = 0;

    if (encoding >= kEncodingCount)
    {
        return Status::Invalid;
    }

    const EncodingRecord& record = kEncodings[encoding];

    switch (record.kind)
    {
    case Kind::Utf8:
        return DecodeUtf8(bytes, count, fatal, final, out, consumed);
    case Kind::Utf16Be:
    case Kind::Utf16Le:
        return DecodeUtf16(bytes, count, record.kind == Kind::Utf16Be, fatal, final, out,
                           consumed);
    case Kind::SingleByte:
    case Kind::XUserDefined:
        return DecodeSingleByte(bytes, count, record.kind, record.table, fatal, out, consumed);
    }

    return Status::Invalid;
}

bool EncodeUtf8(const char16_t* units, Size count, Vector<u8>& out) noexcept
{
    if (out.Reserve(out.Size() + count).HasError())
    {
        return false;
    }

    for (Size i = 0; i < count;)
    {
        Size length = 0;
        const char32_t cp = CodePointAt(units, count, i, length);
        u8 bytes[4];
        const Size n = Utf8Length(cp);
        WriteUtf8(cp, bytes);

        for (Size b = 0; b < n; ++b)
        {
            if (out.PushBack(bytes[b]).HasError())
            {
                return false;
            }
        }

        i += length;
    }

    return true;
}

void EncodeUtf8Into(const char16_t* units, Size count, u8* destination, Size capacity, Size& read,
                    Size& written) noexcept
{
    read = 0;
    written = 0;

    while (read < count)
    {
        Size length = 0;
        const char32_t cp = CodePointAt(units, count, read, length);
        const Size n = Utf8Length(cp);

        if (written + n > capacity)
        {
            return;
        }

        WriteUtf8(cp, destination + written);
        written += n;
        read += length;
    }
}

Status DecodeUtf8(const u8* bytes, Size count, bool fatal, bool final, Vector<char16_t>& out,
                  Size& consumed) noexcept
{
    consumed = 0;

    if (out.Reserve(out.Size() + count).HasError())
    {
        return Status::OutOfMemory;
    }

    // The WHATWG UTF-8 decoder, one byte at a time.
    char32_t codePoint = 0;
    Size bytesNeeded = 0;
    Size bytesSeen = 0;
    u8 lower = 0x80;
    u8 upper = 0xBF;
    Size sequenceStart = 0;

    auto error = [&]() -> Status
    {
        if (fatal)
        {
            return Status::Invalid;
        }

        return out.PushBack(kReplacement).HasValue() ? Status::Ok : Status::OutOfMemory;
    };

    for (Size i = 0; i < count;)
    {
        const u8 b = bytes[i];

        if (bytesNeeded == 0)
        {
            sequenceStart = i;
            ++i;

            if (b <= 0x7F)
            {
                if (out.PushBack(static_cast<char16_t>(b)).HasError())
                {
                    return Status::OutOfMemory;
                }
            }
            else if (b >= 0xC2 && b <= 0xDF)
            {
                bytesNeeded = 1;
                codePoint = b & 0x1F;
            }
            else if (b >= 0xE0 && b <= 0xEF)
            {
                lower = b == 0xE0 ? 0xA0 : 0x80;
                upper = b == 0xED ? 0x9F : 0xBF;
                bytesNeeded = 2;
                codePoint = b & 0x0F;
            }
            else if (b >= 0xF0 && b <= 0xF4)
            {
                lower = b == 0xF0 ? 0x90 : 0x80;
                upper = b == 0xF4 ? 0x8F : 0xBF;
                bytesNeeded = 3;
                codePoint = b & 0x07;
            }
            else if (Status s = error(); s != Status::Ok)
            {
                return s;
            }

            continue;
        }

        if (b < lower || b > upper)
        {
            // The sequence so far is one error; this byte is looked at again.
            codePoint = 0;
            bytesNeeded = 0;
            bytesSeen = 0;
            lower = 0x80;
            upper = 0xBF;

            if (Status s = error(); s != Status::Ok)
            {
                return s;
            }

            continue;
        }

        ++i;
        lower = 0x80;
        upper = 0xBF;
        codePoint = (codePoint << 6) | (b & 0x3F);

        if (++bytesSeen == bytesNeeded)
        {
            if (!AppendCodePoint(codePoint, out))
            {
                return Status::OutOfMemory;
            }

            codePoint = 0;
            bytesNeeded = 0;
            bytesSeen = 0;
        }
    }

    if (bytesNeeded != 0)
    {
        if (!final)
        {
            consumed = sequenceStart;  // keep the incomplete sequence for later
            return Status::Ok;
        }

        if (Status s = error(); s != Status::Ok)
        {
            return s;
        }
    }

    consumed = count;
    return Status::Ok;
}

Status DecodeBase64(const char16_t* chars, Size count, Vector<u8>& out) noexcept
{
    // 1. ASCII whitespace is ignored.
    Vector<char16_t> data;

    if (data.Reserve(count).HasError())
    {
        return Status::OutOfMemory;
    }

    for (Size i = 0; i < count; ++i)
    {
        if (!IsAsciiWhitespace(chars[i]) && data.PushBack(chars[i]).HasError())
        {
            return Status::OutOfMemory;
        }
    }

    Size length = data.Size();

    // 2. One or two '=' of padding are removed when the length is a
    //    multiple of 4.
    if (length % 4 == 0 && length >= 1 && data[length - 1] == '=')
    {
        --length;

        if (length >= 1 && data[length - 1] == '=')
        {
            --length;
        }
    }

    // 3. A length of 4n + 1 cannot be valid.
    if (length % 4 == 1)
    {
        return Status::Invalid;
    }

    if (out.Reserve(out.Size() + length / 4 * 3 + 2).HasError())
    {
        return Status::OutOfMemory;
    }

    // 4.-5. Decode, refusing anything outside the alphabet.
    unsigned buffer = 0;
    unsigned bits = 0;

    for (Size i = 0; i < length; ++i)
    {
        const int value = Base64Value(data[i]);

        if (value < 0)
        {
            return Status::Invalid;
        }

        buffer = (buffer << 6) | static_cast<unsigned>(value);
        bits += 6;

        if (bits == 24)
        {
            if (out.PushBack(static_cast<u8>(buffer >> 16)).HasError() ||
                out.PushBack(static_cast<u8>((buffer >> 8) & 0xFF)).HasError() ||
                out.PushBack(static_cast<u8>(buffer & 0xFF)).HasError())
            {
                return Status::OutOfMemory;
            }

            buffer = 0;
            bits = 0;
        }
    }

    // 6. 12 bits left: one byte (4 discarded); 18 bits: two (2 discarded).
    if (bits == 12)
    {
        if (out.PushBack(static_cast<u8>(buffer >> 4)).HasError())
        {
            return Status::OutOfMemory;
        }
    }
    else if (bits == 18)
    {
        if (out.PushBack(static_cast<u8>(buffer >> 10)).HasError() ||
            out.PushBack(static_cast<u8>((buffer >> 2) & 0xFF)).HasError())
        {
            return Status::OutOfMemory;
        }
    }

    return Status::Ok;
}

Status EncodeBase64(const char16_t* chars, Size count, Vector<char>& out) noexcept
{
    static const char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    for (Size i = 0; i < count; ++i)
    {
        if (chars[i] > 0xFF)
        {
            return Status::Invalid;
        }
    }

    if (out.Reserve(out.Size() + (count + 2) / 3 * 4).HasError())
    {
        return Status::OutOfMemory;
    }

    for (Size i = 0; i < count; i += 3)
    {
        const Size left = count - i;
        const unsigned b0 = chars[i];
        const unsigned b1 = left > 1 ? chars[i + 1] : 0;
        const unsigned b2 = left > 2 ? chars[i + 2] : 0;
        const unsigned triple = (b0 << 16) | (b1 << 8) | b2;
        const char quad[4] = {
            kAlphabet[(triple >> 18) & 0x3F],
            kAlphabet[(triple >> 12) & 0x3F],
            left > 1 ? kAlphabet[(triple >> 6) & 0x3F] : '=',
            left > 2 ? kAlphabet[triple & 0x3F] : '=',
        };

        for (char c : quad)
        {
            if (out.PushBack(c).HasError())
            {
                return Status::OutOfMemory;
            }
        }
    }

    return Status::Ok;
}

} // namespace forge::runtime::encoding
