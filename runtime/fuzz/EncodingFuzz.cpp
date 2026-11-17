// libFuzzer target for runtime/Encoding (Phase 10.1, rule R24). For every
// input, as UTF-8 bytes and as UTF-16 code units:
//
//   1. Decoding never consumes more than it was given, and with `final`
//      consumes everything.
//   2. Decoding in two pieces (the first in stream mode, its unconsumed
//      bytes carried over, as TextDecoder does) gives the same text as
//      decoding at once.
//   3. Fatal decoding succeeds exactly when the input is valid UTF-8, and
//      then gives the same text; re-encoding that text gives the input back.
//   4. Encoding never produces invalid UTF-8, and encodeInto writes a prefix
//      of encode's output that fits.
//   5. base64: what btoa produces, atob decodes back to the same bytes.
//   6. Every other encoding (UTF-16BE/LE, the single-byte ones,
//      x-user-defined): split decoding equals whole decoding, fatal decoding
//      succeeds only with the same text, and label lookup never crashes.
//
// Build and run (clang, from the repository root):
//   clang++ -std=c++17 -g -O1 -fno-exceptions -fsanitize=fuzzer,address,undefined -iquote .
//       runtime/fuzz/EncodingFuzz.cpp runtime/Encoding.cpp
//       forge-core/memory/DefaultAllocator.cpp forge-core/memory/detail/AllocationBackend.cpp
//       -o EncodingFuzz
//   ./EncodingFuzz -max_total_time=60 runtime/fuzz/encoding-corpus

#include "runtime/Encoding.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

using forge::core::Size;
using forge::core::u8;
using forge::core::Vector;
using namespace forge::runtime::encoding;

static void Require(bool condition)
{
    if (!condition)
    {
        abort();
    }
}

static bool SameUnits(const Vector<char16_t>& a, const Vector<char16_t>& b)
{
    return a.Size() == b.Size() &&
           (a.Size() == 0 || memcmp(a.Data(), b.Data(), a.Size() * sizeof(char16_t)) == 0);
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size > 4096)
    {
        return 0;
    }

    const u8* bytes = data;

    // 1.-2. Whole and split decoding agree.
    Vector<char16_t> whole;
    Size consumed = 0;
    Require(DecodeUtf8(bytes, size, false, true, whole, consumed) == Status::Ok && consumed == size);

    const Size split = size == 0 ? 0 : data[0] % (size + 1);
    Vector<char16_t> pieces;
    Require(DecodeUtf8(bytes, split, false, false, pieces, consumed) == Status::Ok);
    Require(consumed <= split && split - consumed <= 3);

    Vector<u8> rest;

    for (Size i = consumed; i < size; ++i)
    {
        Require(rest.PushBack(bytes[i]).HasValue());
    }

    Size restConsumed = 0;
    Require(DecodeUtf8(rest.Data(), rest.Size(), false, true, pieces, restConsumed) == Status::Ok);
    Require(SameUnits(whole, pieces));

    // 3. Fatal decoding, and re-encoding valid input.
    Vector<char16_t> strict;
    const Status fatal = DecodeUtf8(bytes, size, true, true, strict, consumed);
    Vector<u8> reencoded;
    Require(EncodeUtf8(whole.Data(), whole.Size(), reencoded));
    const bool roundTrips = reencoded.Size() == size && (size == 0 || memcmp(reencoded.Data(), bytes, size) == 0);

    if (fatal == Status::Ok)
    {
        Require(SameUnits(strict, whole) && roundTrips);
    }
    else
    {
        Require(fatal == Status::Invalid && !roundTrips);
    }

    // 4. The input as UTF-16 code units: encoding gives valid UTF-8, and
    //    encodeInto a fitting prefix.
    const Size unitCount = size / 2;
    Vector<char16_t> units;

    for (Size i = 0; i < unitCount; ++i)
    {
        Require(units.PushBack(static_cast<char16_t>(data[2 * i] | (data[2 * i + 1] << 8))).HasValue());
    }

    Vector<u8> encoded;
    Require(EncodeUtf8(units.Data(), units.Size(), encoded));
    Vector<char16_t> back;
    Require(DecodeUtf8(encoded.Data(), encoded.Size(), true, true, back, consumed) == Status::Ok);

    u8 buffer[64];
    const Size capacity = size == 0 ? 0 : data[size - 1] % sizeof(buffer);
    Size read = 0;
    Size written = 0;
    EncodeUtf8Into(units.Data(), units.Size(), buffer, capacity, read, written);
    Require(read <= units.Size() && written <= capacity && written <= encoded.Size());
    Require(written == 0 || memcmp(buffer, encoded.Data(), written) == 0);

    // 5. btoa then atob, on the bytes as Latin-1 code units.
    Vector<char16_t> latin1;

    for (Size i = 0; i < size; ++i)
    {
        Require(latin1.PushBack(bytes[i]).HasValue());
    }

    Vector<char> base64;
    Require(EncodeBase64(latin1.Data(), latin1.Size(), base64) == Status::Ok);
    Vector<char16_t> base64Units;

    for (Size i = 0; i < base64.Size(); ++i)
    {
        Require(base64Units.PushBack(static_cast<char16_t>(base64[i])).HasValue());
    }

    Vector<u8> decoded;
    Require(DecodeBase64(base64Units.Data(), base64Units.Size(), decoded) == Status::Ok);
    Require(decoded.Size() == size && (size == 0 || memcmp(decoded.Data(), bytes, size) == 0));

    // Any input as base64 text must not crash.
    Vector<u8> anything;
    (void)DecodeBase64(units.Data(), units.Size(), anything);

    // 6. The other encodings, chosen by the last byte.
    const Size encoding = size == 0 ? 1 : 1 + data[size - 1] % (EncodingCount() - 1);
    Vector<char16_t> otherWhole;
    Require(Decode(encoding, bytes, size, false, true, otherWhole, consumed) == Status::Ok &&
            consumed == size);
    Vector<char16_t> otherPieces;
    Require(Decode(encoding, bytes, split, false, false, otherPieces, consumed) == Status::Ok);
    Require(consumed <= split && split - consumed <= 3);
    Vector<u8> otherRest;

    for (Size i = consumed; i < size; ++i)
    {
        Require(otherRest.PushBack(bytes[i]).HasValue());
    }

    Require(Decode(encoding, otherRest.Data(), otherRest.Size(), false, true, otherPieces,
                   restConsumed) == Status::Ok);
    Require(SameUnits(otherWhole, otherPieces));
    Vector<char16_t> otherStrict;
    const Status otherFatal = Decode(encoding, bytes, size, true, true, otherStrict, consumed);
    Require(otherFatal == Status::Invalid || (otherFatal == Status::Ok && SameUnits(otherStrict, otherWhole)));

    for (Size i = 0; i < otherWhole.Size(); ++i)
    {
        Require(otherFatal != Status::Ok || otherWhole[i] != 0xFFFD ||
                EncodingName(encoding)[0] == 'u');  // only UTF-16 can decode U+FFFD itself
    }

    Size found = 0;
    (void)FindEncoding(units.Data(), units.Size(), found);
    Require(found < EncodingCount());
    return 0;
}
