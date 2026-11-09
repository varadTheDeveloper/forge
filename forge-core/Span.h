#pragma once

#include "Assert.h"
#include "Types.h"

namespace forge::core
{

/// Non-owning view over a contiguous range of T. Never allocates, never
/// outlives the buffer it points into — same relationship to that buffer
/// as a raw pointer + length, just with bounds and a small, safe API
/// instead of two loose variables passed around separately.
///
/// Mutable (Span<T>) or read-only (Span<const T>) depending on T — same
/// convention as std::span, just without the C++20 dependency this
/// codebase's real build target can't rely on (see HISTORY.md /
/// AGENTS.md).
template<typename T>
class Span
{
public:

    using SizeType = forge::core::Size;

    constexpr Span() noexcept = default;

    constexpr Span(
        T* data,
        SizeType size) noexcept
        :
        data_(data),
        size_(size)
    {
    }

    [[nodiscard]]
    constexpr T* Data() const noexcept
    {
        return data_;
    }

    [[nodiscard]]
    constexpr SizeType Size() const noexcept
    {
        return size_;
    }

    [[nodiscard]]
    constexpr bool Empty() const noexcept
    {
        return size_ == 0;
    }

    [[nodiscard]]
    constexpr T& operator[](
        SizeType index) const noexcept
    {
        FORGE_RELEASE_ASSERT(index < size_);
        return data_[index];
    }

    [[nodiscard]]
    constexpr T& Front() const noexcept
    {
        FORGE_RELEASE_ASSERT(size_ > 0);
        return data_[0];
    }

    [[nodiscard]]
    constexpr T& Back() const noexcept
    {
        FORGE_RELEASE_ASSERT(size_ > 0);
        return data_[size_ - 1];
    }

    /// A view over [offset, offset + count), clamped so it never reads
    /// past this span's own end even if `count` (or `offset`) is too
    /// large — callers that want to detect an out-of-range request should
    /// check `offset <= Size()` themselves first; this exists so a
    /// slightly-too-generous count (a common, harmless mistake — e.g.
    /// "give me the rest" via SIZE_MAX) doesn't read out of bounds.
    [[nodiscard]]
    constexpr Span Subspan(
        SizeType offset,
        SizeType count) const noexcept
    {
        if (offset >= size_)
        {
            return Span(data_ + size_, 0);
        }

        const SizeType available = size_ - offset;

        return Span(data_ + offset, count < available ? count : available);
    }

    // Raw-pointer iterators: enough for range-for and standard algorithms,
    // without depending on anything beyond the language itself.
    [[nodiscard]] constexpr T* begin() const noexcept { return data_; }
    [[nodiscard]] constexpr T* end() const noexcept { return data_ + size_; }

private:

    T* data_{ nullptr };
    SizeType size_{ 0 };
};

} // namespace forge::core
