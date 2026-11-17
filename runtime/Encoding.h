#pragma once

// Text and base64 encodings for the web APIs (Phase 10.1, PHASE10_DESIGN.md).
//
// Portable code with no JSAPI, unit-tested in both CI test jobs
// (runtime/EncodingTest.cpp) and fuzzed (runtime/fuzz/EncodingFuzz.cpp).
// JavaScript strings are sequences of UTF-16 code units, so the string side
// of every function is char16_t.
//
//   UTF-8 (WHATWG Encoding Standard, "UTF-8 decoder" and "UTF-8 encode"):
//     decoding replaces each maximal invalid subpart with one U+FFFD (or
//     stops, in fatal mode); encoding replaces lone surrogates with U+FFFD.
//   Other decoders (TextDecoder): UTF-16BE, UTF-16LE, x-user-defined and
//     every single-byte encoding of the standard, with tables generated from
//     the standard's own index files (runtime/EncodingTables.inc). The
//     multi-byte CJK encodings are not supported yet (PHASE10_DESIGN.md).
//   Base64 (HTML Standard, "forgiving-base64 decode" and btoa):
//     decoding ignores ASCII whitespace and accepts missing padding;
//     encoding takes one byte per code unit and refuses units above 0xFF.

#include "forge-core/Types.h"
#include "forge-core/memory/Vector.h"

namespace forge::runtime::encoding
{

/// Encodes UTF-16 `units` as UTF-8, appending to `out`. A lone surrogate
/// becomes U+FFFD. False only when out of memory.
[[nodiscard]]
bool EncodeUtf8(
    const char16_t* units,
    forge::core::Size count,
    forge::core::Vector<forge::core::u8>& out) noexcept;

/// TextEncoder.encodeInto: encodes as many whole code points of `units` as
/// fit in `capacity` bytes. `read` receives the code units consumed and
/// `written` the bytes written.
void EncodeUtf8Into(
    const char16_t* units,
    forge::core::Size count,
    forge::core::u8* destination,
    forge::core::Size capacity,
    forge::core::Size& read,
    forge::core::Size& written) noexcept;

enum class Status
{
    Ok,
    Invalid,      // fatal decoding met an error; btoa met a unit above 0xFF;
                  // base64 input is not valid
    OutOfMemory,
};

/// Decodes UTF-8 `bytes`, appending UTF-16 code units to `out`.
///   fatal: stop with Status::Invalid at the first error (nothing is
///          appended for the invalid sequence); otherwise each maximal
///          invalid subpart becomes one U+FFFD.
///   final: the input ends here. Without it, an incomplete sequence at the
///          end is left unconsumed, so `consumed` may be less than `count`
///          (TextDecoder's stream mode).
[[nodiscard]]
Status DecodeUtf8(
    const forge::core::u8* bytes,
    forge::core::Size count,
    bool fatal,
    bool final,
    forge::core::Vector<char16_t>& out,
    forge::core::Size& consumed) noexcept;

/// The encoding numbers Decode takes. kUtf8 is always 0.
constexpr forge::core::Size kUtf8 = 0;

enum class LabelLookup
{
    Supported,    // `encoding` is set
    Replacement,  // a label of the "replacement" encoding (TextDecoder refuses it)
    Unsupported,  // a label of a CJK multi-byte encoding Forge does not decode yet
    Unknown,      // not a label
};

/// The WHATWG "get an encoding" steps: ASCII whitespace around `label` is
/// ignored and ASCII letters match in any case.
LabelLookup FindEncoding(
    const char16_t* label,
    forge::core::Size length,
    forge::core::Size& encoding) noexcept;

/// The number of supported encodings (valid numbers are below it).
forge::core::Size EncodingCount() noexcept;

/// The encoding's name in lower case, as TextDecoder.encoding reports it
/// ("utf-8", "windows-1252", "utf-16le", ...).
const char* EncodingName(
    forge::core::Size encoding) noexcept;

/// Decodes `bytes` in `encoding`, with the same `fatal`, `final` and
/// `consumed` rules as DecodeUtf8 (a UTF-16 decoder keeps an odd byte or an
/// unpaired lead surrogate at the end for later; a single-byte decoder always
/// consumes everything). A byte-order mark is decoded, not removed.
[[nodiscard]]
Status Decode(
    forge::core::Size encoding,
    const forge::core::u8* bytes,
    forge::core::Size count,
    bool fatal,
    bool final,
    forge::core::Vector<char16_t>& out,
    forge::core::Size& consumed) noexcept;

/// atob: forgiving-base64 decode of `chars`, appending the bytes to `out`.
[[nodiscard]]
Status DecodeBase64(
    const char16_t* chars,
    forge::core::Size count,
    forge::core::Vector<forge::core::u8>& out) noexcept;

/// btoa: base64 of the bytes `chars` stands for (each code unit must be at
/// most 0xFF), appending ASCII to `out`.
[[nodiscard]]
Status EncodeBase64(
    const char16_t* chars,
    forge::core::Size count,
    forge::core::Vector<char>& out) noexcept;

} // namespace forge::runtime::encoding
