// Strict JSON parser (Phase 9.2). See Json.h.

#include "runtime/Json.h"

#include <utility>

namespace forge::runtime::json
{

using forge::core::Size;
using forge::core::String;
using forge::core::StringView;
using forge::core::u32;
using forge::core::u8;

const Node* Document::Member(const Node& object, StringView key) const noexcept
{
    if (object.kind != Kind::Object)
    {
        return nullptr;
    }

    for (Size i = 0; i < object.keys.Size(); ++i)
    {
        if (object.keys[i].View() == key)
        {
            return &nodes_[object.children[i]];
        }
    }

    return nullptr;
}

class Parser
{
public:
    Parser(StringView input, Document& doc, ParseError& error) noexcept
        : input_(input), doc_(doc), error_(error)
    {
    }

    bool Run() noexcept
    {
        // A UTF-8 byte-order mark is not part of the text.
        if (input_.Size() >= 3 && Byte(0) == 0xEF && Byte(1) == 0xBB && Byte(2) == 0xBF)
        {
            pos_ = 3;
        }

        if (input_.Size() > kMaxInput)
        {
            return Fail("the file is larger than 1 MiB");
        }

        Size root = 0;

        if (!NewNode(root) || !ParseValue(root, 0))
        {
            return false;
        }

        SkipWhitespace();

        if (pos_ != input_.Size())
        {
            return Fail("unexpected text after the end of the JSON value");
        }

        return true;
    }

private:
    u8 Byte(Size at) const noexcept { return static_cast<u8>(input_.Data()[at]); }

    bool Fail(const char* message) noexcept
    {
        if (error_.message == nullptr)
        {
            error_.message = message;
            error_.offset = pos_;
        }

        return false;
    }

    bool NewNode(Size& index) noexcept
    {
        if (doc_.nodes_.Size() >= kMaxNodes)
        {
            return Fail("too many values (the limit is 100000)");
        }

        index = doc_.nodes_.Size();

        if (doc_.nodes_.PushBack(Node()).HasError())
        {
            return Fail("out of memory");
        }

        return true;
    }

    void SkipWhitespace() noexcept
    {
        while (pos_ < input_.Size())
        {
            const u8 c = Byte(pos_);

            if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
            {
                return;
            }

            ++pos_;
        }
    }

    bool Literal(const char* word) noexcept
    {
        const StringView w(word);

        if (input_.Size() - pos_ < w.Size())
        {
            return false;
        }

        for (Size i = 0; i < w.Size(); ++i)
        {
            if (input_.Data()[pos_ + i] != w.Data()[i])
            {
                return false;
            }
        }

        pos_ += w.Size();
        return true;
    }

    bool ParseValue(Size index, Size depth) noexcept
    {
        if (depth > kMaxDepth)
        {
            return Fail("nested more than 64 levels deep");
        }

        SkipWhitespace();

        if (pos_ >= input_.Size())
        {
            return Fail("unexpected end of the file");
        }

        const u8 c = Byte(pos_);

        switch (c)
        {
        case '{':
            return ParseObject(index, depth);
        case '[':
            return ParseArray(index, depth);
        case '"':
        {
            doc_.nodes_[index].kind = Kind::String;
            String text;

            if (!ParseString(text))
            {
                return false;
            }

            doc_.nodes_[index].text.Swap(text);
            return true;
        }
        case 't':
            doc_.nodes_[index].kind = Kind::True;
            return Literal("true") || Fail("expected a JSON value");
        case 'f':
            doc_.nodes_[index].kind = Kind::False;
            return Literal("false") || Fail("expected a JSON value");
        case 'n':
            doc_.nodes_[index].kind = Kind::Null;
            return Literal("null") || Fail("expected a JSON value");
        default:
            if (c == '-' || (c >= '0' && c <= '9'))
            {
                return ParseNumber(index);
            }

            return Fail("expected a JSON value");
        }
    }

    bool ParseNumber(Size index) noexcept
    {
        const Size start = pos_;
        auto digit = [&](Size at) { return at < input_.Size() && Byte(at) >= '0' && Byte(at) <= '9'; };

        if (pos_ < input_.Size() && Byte(pos_) == '-')
        {
            ++pos_;
        }

        if (!digit(pos_))
        {
            return Fail("a number must start with a digit");
        }

        if (Byte(pos_) == '0')
        {
            ++pos_;

            if (digit(pos_))
            {
                return Fail("a number must not have a leading zero");
            }
        }
        else
        {
            while (digit(pos_))
            {
                ++pos_;
            }
        }

        if (pos_ < input_.Size() && Byte(pos_) == '.')
        {
            ++pos_;

            if (!digit(pos_))
            {
                return Fail("a decimal point must be followed by a digit");
            }

            while (digit(pos_))
            {
                ++pos_;
            }
        }

        if (pos_ < input_.Size() && (Byte(pos_) == 'e' || Byte(pos_) == 'E'))
        {
            ++pos_;

            if (pos_ < input_.Size() && (Byte(pos_) == '+' || Byte(pos_) == '-'))
            {
                ++pos_;
            }

            if (!digit(pos_))
            {
                return Fail("an exponent must have a digit");
            }

            while (digit(pos_))
            {
                ++pos_;
            }
        }

        Node& node = doc_.nodes_[index];
        node.kind = Kind::Number;

        if (node.text.Append(StringView(input_.Data() + start, pos_ - start)).HasError())
        {
            return Fail("out of memory");
        }

        return true;
    }

    bool AppendCodePoint(String& out, u32 cp) noexcept
    {
        char bytes[4];
        Size n = 0;

        if (cp < 0x80)
        {
            bytes[n++] = static_cast<char>(cp);
        }
        else if (cp < 0x800)
        {
            bytes[n++] = static_cast<char>(0xC0 | (cp >> 6));
            bytes[n++] = static_cast<char>(0x80 | (cp & 0x3F));
        }
        else if (cp < 0x10000)
        {
            bytes[n++] = static_cast<char>(0xE0 | (cp >> 12));
            bytes[n++] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            bytes[n++] = static_cast<char>(0x80 | (cp & 0x3F));
        }
        else
        {
            bytes[n++] = static_cast<char>(0xF0 | (cp >> 18));
            bytes[n++] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            bytes[n++] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            bytes[n++] = static_cast<char>(0x80 | (cp & 0x3F));
        }

        return out.Append(StringView(bytes, n)).HasValue() || Fail("out of memory");
    }

    bool ReadHex4(u32& value) noexcept
    {
        if (input_.Size() - pos_ < 4)
        {
            return Fail("a \\u escape needs four hexadecimal digits");
        }

        value = 0;

        for (Size i = 0; i < 4; ++i)
        {
            const u8 c = Byte(pos_ + i);
            u32 d;

            if (c >= '0' && c <= '9')
            {
                d = c - '0';
            }
            else if (c >= 'a' && c <= 'f')
            {
                d = c - 'a' + 10;
            }
            else if (c >= 'A' && c <= 'F')
            {
                d = c - 'A' + 10;
            }
            else
            {
                return Fail("a \\u escape needs four hexadecimal digits");
            }

            value = value * 16 + d;
        }

        pos_ += 4;
        return true;
    }

    /// Copies one raw UTF-8 sequence starting at pos_, validating it
    /// (shortest form, no surrogates, at most U+10FFFF).
    bool CopyUtf8(String& out) noexcept
    {
        const u8 c = Byte(pos_);
        Size length;
        u32 cp;

        if (c < 0x80)
        {
            length = 1;
            cp = c;
        }
        else if ((c & 0xE0) == 0xC0)
        {
            length = 2;
            cp = c & 0x1F;
        }
        else if ((c & 0xF0) == 0xE0)
        {
            length = 3;
            cp = c & 0x0F;
        }
        else if ((c & 0xF8) == 0xF0)
        {
            length = 4;
            cp = c & 0x07;
        }
        else
        {
            return Fail("the file is not valid UTF-8");
        }

        if (input_.Size() - pos_ < length)
        {
            return Fail("the file is not valid UTF-8");
        }

        for (Size i = 1; i < length; ++i)
        {
            const u8 next = Byte(pos_ + i);

            if ((next & 0xC0) != 0x80)
            {
                return Fail("the file is not valid UTF-8");
            }

            cp = (cp << 6) | (next & 0x3F);
        }

        static const u32 kMin[] = { 0, 0, 0x80, 0x800, 0x10000 };

        if (cp < kMin[length] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
        {
            return Fail("the file is not valid UTF-8");
        }

        if (out.Append(StringView(input_.Data() + pos_, length)).HasError())
        {
            return Fail("out of memory");
        }

        pos_ += length;
        return true;
    }

    bool ParseString(String& out) noexcept
    {
        ++pos_;  // the opening quote

        for (;;)
        {
            if (pos_ >= input_.Size())
            {
                return Fail("a string is not closed");
            }

            const u8 c = Byte(pos_);

            if (c == '"')
            {
                ++pos_;
                return true;
            }

            if (c < 0x20)
            {
                return Fail("a control character must be escaped inside a string");
            }

            if (c != '\\')
            {
                if (!CopyUtf8(out))
                {
                    return false;
                }

                continue;
            }

            ++pos_;

            if (pos_ >= input_.Size())
            {
                return Fail("a string is not closed");
            }

            const u8 e = Byte(pos_++);
            char simple = 0;

            switch (e)
            {
            case '"': simple = '"'; break;
            case '\\': simple = '\\'; break;
            case '/': simple = '/'; break;
            case 'b': simple = '\b'; break;
            case 'f': simple = '\f'; break;
            case 'n': simple = '\n'; break;
            case 'r': simple = '\r'; break;
            case 't': simple = '\t'; break;
            case 'u':
            {
                u32 cp = 0;

                if (!ReadHex4(cp))
                {
                    return false;
                }

                if (cp >= 0xDC00 && cp <= 0xDFFF)
                {
                    return Fail("a \\u escape leaves a lone surrogate");
                }

                if (cp >= 0xD800 && cp <= 0xDBFF)
                {
                    u32 low = 0;

                    if (input_.Size() - pos_ < 2 || Byte(pos_) != '\\' || Byte(pos_ + 1) != 'u')
                    {
                        return Fail("a \\u escape leaves a lone surrogate");
                    }

                    pos_ += 2;

                    if (!ReadHex4(low))
                    {
                        return false;
                    }

                    if (low < 0xDC00 || low > 0xDFFF)
                    {
                        return Fail("a \\u escape leaves a lone surrogate");
                    }

                    cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                }

                if (!AppendCodePoint(out, cp))
                {
                    return false;
                }

                continue;
            }
            default:
                return Fail("unknown escape in a string");
            }

            if (out.Append(simple).HasError())
            {
                return Fail("out of memory");
            }
        }
    }

    bool ParseArray(Size index, Size depth) noexcept
    {
        doc_.nodes_[index].kind = Kind::Array;
        ++pos_;
        SkipWhitespace();

        if (pos_ < input_.Size() && Byte(pos_) == ']')
        {
            ++pos_;
            return true;
        }

        for (;;)
        {
            if (doc_.nodes_[index].children.Size() >= kMaxMembers)
            {
                return Fail("an array has more than 4096 items");
            }

            Size child = 0;

            if (!NewNode(child) || doc_.nodes_[index].children.PushBack(child).HasError())
            {
                return Fail("out of memory");
            }

            if (!ParseValue(child, depth + 1))
            {
                return false;
            }

            SkipWhitespace();

            if (pos_ >= input_.Size())
            {
                return Fail("an array is not closed");
            }

            if (Byte(pos_) == ',')
            {
                ++pos_;
                SkipWhitespace();

                if (pos_ < input_.Size() && Byte(pos_) == ']')
                {
                    return Fail("a trailing comma is not allowed");
                }

                continue;
            }

            if (Byte(pos_) == ']')
            {
                ++pos_;
                return true;
            }

            return Fail("expected ',' or ']' in an array");
        }
    }

    bool ParseObject(Size index, Size depth) noexcept
    {
        doc_.nodes_[index].kind = Kind::Object;
        ++pos_;
        SkipWhitespace();

        if (pos_ < input_.Size() && Byte(pos_) == '}')
        {
            ++pos_;
            return true;
        }

        for (;;)
        {
            SkipWhitespace();

            if (pos_ >= input_.Size() || Byte(pos_) != '"')
            {
                return Fail("expected a quoted key in an object");
            }

            String key;

            if (!ParseString(key))
            {
                return false;
            }

            {
                const Node& object = doc_.nodes_[index];

                if (object.keys.Size() >= kMaxMembers)
                {
                    return Fail("an object has more than 4096 members");
                }

                for (Size i = 0; i < object.keys.Size(); ++i)
                {
                    if (object.keys[i].View() == key.View())
                    {
                        return Fail("a key appears twice in one object");
                    }
                }
            }

            SkipWhitespace();

            if (pos_ >= input_.Size() || Byte(pos_) != ':')
            {
                return Fail("expected ':' after a key");
            }

            ++pos_;
            Size child = 0;

            if (!NewNode(child))
            {
                return false;
            }

            if (doc_.nodes_[index].keys.PushBack(std::move(key)).HasError() ||
                doc_.nodes_[index].children.PushBack(child).HasError())
            {
                return Fail("out of memory");
            }

            if (!ParseValue(child, depth + 1))
            {
                return false;
            }

            SkipWhitespace();

            if (pos_ >= input_.Size())
            {
                return Fail("an object is not closed");
            }

            if (Byte(pos_) == ',')
            {
                ++pos_;
                SkipWhitespace();

                if (pos_ < input_.Size() && Byte(pos_) == '}')
                {
                    return Fail("a trailing comma is not allowed");
                }

                continue;
            }

            if (Byte(pos_) == '}')
            {
                ++pos_;
                return true;
            }

            return Fail("expected ',' or '}' in an object");
        }
    }

    StringView input_;
    Document& doc_;
    ParseError& error_;
    Size pos_ = 0;
};

bool Parse(StringView input, Document& out, ParseError& error) noexcept
{
    error = ParseError();
    out = Document();
    Parser parser(input, out, error);
    return parser.Run();
}

} // namespace forge::runtime::json
