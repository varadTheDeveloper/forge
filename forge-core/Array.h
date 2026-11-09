#pragma once

#include <type_traits>
#include <utility>

#include "Assert.h"
#include "Types.h"

namespace forge::core
{

/// Fixed-size, stack-allocated array of exactly N elements of type T. Never
/// allocates, never grows/shrinks — unlike Vector<T>, there is no
/// size/capacity distinction and therefore no invariant for this type to
/// protect, which is why (like std::array, and unlike every other
/// container in forge-core) `data_` is a public member and Array is a
/// plain aggregate: `Array<int, 3> a{{1, 2, 3}};` works directly.
///
/// N == 0 is handled by the specialization below (a zero-length C array
/// is not valid C++, so it can't just fall out of the primary template).
template<typename T, forge::core::Size N>
class Array
{
public:

    using SizeType = forge::core::Size;

    // Intentionally public — see the class comment above.
    T data_[N];

    [[nodiscard]]
    constexpr SizeType Size() const noexcept
    {
        return N;
    }

    [[nodiscard]]
    constexpr bool Empty() const noexcept
    {
        return N == 0;
    }

    [[nodiscard]]
    constexpr T& operator[](
        SizeType index) noexcept
    {
        FORGE_RELEASE_ASSERT(index < N);
        return data_[index];
    }

    [[nodiscard]]
    constexpr const T& operator[](
        SizeType index) const noexcept
    {
        FORGE_RELEASE_ASSERT(index < N);
        return data_[index];
    }

    [[nodiscard]]
    constexpr T& Front() noexcept
    {
        return data_[0];
    }

    [[nodiscard]]
    constexpr const T& Front() const noexcept
    {
        return data_[0];
    }

    [[nodiscard]]
    constexpr T& Back() noexcept
    {
        return data_[N - 1];
    }

    [[nodiscard]]
    constexpr const T& Back() const noexcept
    {
        return data_[N - 1];
    }

    [[nodiscard]]
    constexpr T* Data() noexcept
    {
        return data_;
    }

    [[nodiscard]]
    constexpr const T* Data() const noexcept
    {
        return data_;
    }

    constexpr void Fill(
        const T& value) noexcept(std::is_nothrow_copy_assignable_v<T>)
    {
        for (SizeType index = 0; index < N; ++index)
        {
            data_[index] = value;
        }
    }

    constexpr void Swap(
        Array& other) noexcept(std::is_nothrow_swappable_v<T>)
    {
        using std::swap;

        for (SizeType index = 0; index < N; ++index)
        {
            swap(data_[index], other.data_[index]);
        }
    }

    [[nodiscard]] constexpr T* begin() noexcept { return data_; }
    [[nodiscard]] constexpr T* end() noexcept { return data_ + N; }
    [[nodiscard]] constexpr const T* begin() const noexcept { return data_; }
    [[nodiscard]] constexpr const T* end() const noexcept { return data_ + N; }

    [[nodiscard]]
    friend constexpr bool operator==(
        const Array& lhs,
        const Array& rhs) noexcept
    {
        for (SizeType index = 0; index < N; ++index)
        {
            if (!(lhs.data_[index] == rhs.data_[index]))
            {
                return false;
            }
        }

        return true;
    }

    [[nodiscard]]
    friend constexpr bool operator!=(
        const Array& lhs,
        const Array& rhs) noexcept
    {
        return !(lhs == rhs);
    }
};

/// N == 0 specialization: a zero-length C array (`T data_[0]`) is not
/// valid C++, so this holds no storage at all rather than trying to force
/// the primary template's shape to work for N == 0. Front()/Back() are
/// deliberately omitted — there is no valid index into an empty Array, so
/// unlike the primary template there is nothing safe for them to return.
template<typename T>
class Array<T, 0>
{
public:

    using SizeType = forge::core::Size;

    [[nodiscard]]
    constexpr SizeType Size() const noexcept
    {
        return 0;
    }

    [[nodiscard]]
    constexpr bool Empty() const noexcept
    {
        return true;
    }

    [[nodiscard]]
    constexpr T* Data() noexcept
    {
        return nullptr;
    }

    [[nodiscard]]
    constexpr const T* Data() const noexcept
    {
        return nullptr;
    }

    constexpr void Fill(
        const T&) noexcept
    {
    }

    constexpr void Swap(
        Array&) noexcept
    {
    }

    [[nodiscard]] constexpr T* begin() noexcept { return nullptr; }
    [[nodiscard]] constexpr T* end() noexcept { return nullptr; }
    [[nodiscard]] constexpr const T* begin() const noexcept { return nullptr; }
    [[nodiscard]] constexpr const T* end() const noexcept { return nullptr; }

    [[nodiscard]]
    friend constexpr bool operator==(
        const Array&,
        const Array&) noexcept
    {
        return true;
    }

    [[nodiscard]]
    friend constexpr bool operator!=(
        const Array& lhs,
        const Array& rhs) noexcept
    {
        return !(lhs == rhs);
    }
};

} // namespace forge::core
