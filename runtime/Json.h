#pragma once

// A strict JSON parser for untrusted configuration such as forge.json
// (Phase 9.2). Portable string code with no JSAPI: the manifest of a
// dependency is untrusted input, so it is parsed by small, fuzzed C++
// (decision D5) instead of the engine, and the result is plain data.
//
// Strict RFC 8259, plus these refusals, each of which another parser might
// read differently (so a manifest could mean two things):
//   * duplicate keys in one object;
//   * invalid UTF-8, and \u escapes that leave a lone surrogate;
//   * comments, trailing commas, single quotes, NaN/Infinity, leading '+';
//   * more than kMaxDepth levels of nesting, kMaxMembers members in one
//     object or array, kMaxNodes values in total, or kMaxInput bytes.
// A UTF-8 byte-order mark at the start is skipped. Object member order is
// kept (package "exports" conditions depend on it).

#include "forge-core/String.h"
#include "forge-core/StringView.h"
#include "forge-core/Types.h"
#include "forge-core/memory/Vector.h"

namespace forge::runtime::json
{

constexpr forge::core::Size kMaxInput = 1024 * 1024;
constexpr forge::core::Size kMaxDepth = 64;
constexpr forge::core::Size kMaxMembers = 4096;
constexpr forge::core::Size kMaxNodes = 100000;

enum class Kind : forge::core::u8
{
    Null,
    False,
    True,
    Number,  // `text` holds the number exactly as written
    String,  // `text` holds the decoded UTF-8 string
    Array,
    Object,
};

/// One value. Children of an array or object are indexes into the
/// document's node list; for an object, `keys[i]` names `children[i]`.
struct Node
{
    Kind kind = Kind::Null;
    forge::core::String text;
    forge::core::Vector<forge::core::Size> children;
    forge::core::Vector<forge::core::String> keys;
};

class Document
{
public:
    /// The top-level value. Only valid after a successful Parse.
    const Node& Root() const noexcept { return nodes_[0]; }
    const Node& At(forge::core::Size index) const noexcept { return nodes_[index]; }

    /// The member `key` of `object`, or null if it has none (or `object` is
    /// not an object).
    const Node* Member(const Node& object, forge::core::StringView key) const noexcept;

private:
    friend class Parser;
    forge::core::Vector<Node> nodes_;
};

struct ParseError
{
    const char* message = nullptr;  // plain language, never quotes the input
    forge::core::Size offset = 0;   // byte offset into the input
};

/// Parses `input` into `out`. On failure fills `error` and returns false;
/// `out` is then unusable.
[[nodiscard]]
bool Parse(
    forge::core::StringView input,
    Document& out,
    ParseError& error) noexcept;

} // namespace forge::runtime::json
