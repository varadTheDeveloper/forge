#pragma once

#include <cstring>
#include <utility>

#include "Assert.h"
#include "CheckedMath.h"

namespace forge::core
{

// Largest character count a String may hold: one byte is always reserved
// for the null terminator (Phase 7.9).
constexpr Size kStringMaxCapacity = MaxElementCount<char>() - 1;

//==============================================================================
// Helpers
//==============================================================================

constexpr String::SizeType
String::AllocationSize(
    SizeType capacity) noexcept
{
    // +1: one extra byte reserved for the null terminator, always, on top
    // of whatever `capacity` data characters are being asked for.
    return capacity + 1;
}

inline String::SizeType
String::NextCapacity() const noexcept
{
    // Saturates instead of wrapping (Phase 7.9).
    return GrowCapacity(capacity_, kStringMaxCapacity);
}

//==============================================================================
// Construction
//==============================================================================

inline String::String() noexcept
    :
    data_(nullptr),
    size_(0),
    capacity_(0),
    allocator_(&memory::GetDefaultAllocator())
{
}

inline String::String(
    memory::Allocator& allocator) noexcept
    :
    data_(nullptr),
    size_(0),
    capacity_(0),
    allocator_(&allocator)
{
}

inline String::~String() noexcept
{
    Clear();

    if (data_ != nullptr)
    {
        allocator_->Deallocate(
            data_,
            AllocationSize(capacity_),
            alignof(char));
    }
}

inline Result<String> String::Create(
    memory::Allocator& allocator,
    StringView initial)
{
    String result(allocator);

    if (Result<void> appended = result.Append(initial); appended.HasError())
    {
        return Result<String>(Failure{ appended.Error() });
    }

    return Result<String>(std::move(result));
}

inline Result<String> String::Create(
    StringView initial)
{
    return Create(memory::GetDefaultAllocator(), initial);
}

//==============================================================================
// Capacity
//==============================================================================

inline String::SizeType
String::Size() const noexcept
{
    return size_;
}

inline String::SizeType
String::Capacity() const noexcept
{
    return capacity_;
}

inline bool String::Empty() const noexcept
{
    return size_ == 0;
}

inline Result<void> String::Reserve(
    SizeType capacity)
{
    if (capacity <= capacity_)
    {
        return {};
    }

    // Phase 7.9: AllocationSize() adds 1, which would wrap at SIZE_MAX.
    if (capacity > kStringMaxCapacity)
    {
        return Result<void>(
            Failure{ Error(ErrorCode::Overflow) });
    }

    void* memory = allocator_->Allocate(
        AllocationSize(capacity),
        alignof(char));

    if (memory == nullptr)
    {
        return Result<void>(
            Failure{ Error(ErrorCode::OutOfMemory) });
    }

    char* newData = static_cast<char*>(memory);

    // char has no constructor/destructor semantics worth running, unlike
    // Vector<T>'s arbitrary T — a raw byte copy is correct and simpler.
    if (size_ > 0)
    {
        std::memcpy(newData, data_, size_);
    }

    newData[size_] = '\0';

    if (data_ != nullptr)
    {
        allocator_->Deallocate(
            data_,
            AllocationSize(capacity_),
            alignof(char));
    }

    data_ = newData;
    capacity_ = capacity;

    return {};
}

inline Result<void> String::GrowIfNeeded()
{
    if (size_ < capacity_)
    {
        return {};
    }

    const SizeType next = NextCapacity();

    if (next <= capacity_)
    {
        return Result<void>(
            Failure{ Error(ErrorCode::Overflow) });
    }

    return Reserve(next);
}

inline void String::Clear() noexcept
{
    size_ = 0;

    if (data_ != nullptr)
    {
        data_[0] = '\0';
    }
}

//==============================================================================
// Element Access
//==============================================================================

inline char& String::operator[](
    SizeType index) noexcept
{
    FORGE_RELEASE_ASSERT(index < size_);
    return data_[index];
}

inline const char& String::operator[](
    SizeType index) const noexcept
{
    FORGE_RELEASE_ASSERT(index < size_);
    return data_[index];
}

inline char* String::Data() noexcept
{
    return data_;
}

inline const char* String::Data() const noexcept
{
    return data_;
}

inline const char* String::CStr() const noexcept
{
    // data_[size_] is always '\0' whenever data_ is non-null (Reserve/
    // Append/Clear all maintain this) — the only case needing a fallback
    // is a String that has never allocated at all.
    static constexpr char kEmpty[1] = { '\0' };

    return data_ != nullptr ? data_ : kEmpty;
}

inline StringView String::View() const noexcept
{
    return StringView(data_, size_);
}

inline String::operator StringView() const noexcept
{
    return View();
}

//==============================================================================
// Modifiers
//==============================================================================

inline Result<void> String::Append(
    char c)
{
    if (Result<void> result = GrowIfNeeded(); result.HasError())
    {
        return result;
    }

    data_[size_] = c;
    ++size_;
    data_[size_] = '\0';

    return {};
}

inline Result<void> String::Append(
    StringView text)
{
    if (text.Empty())
    {
        return {};
    }

    // Phase 7.9: both the sum and the doubling loop used to wrap silently.
    SizeType required = 0;

    if (!CheckedAdd(size_, text.Size(), &required) || required > kStringMaxCapacity)
    {
        return Result<void>(
            Failure{ Error(ErrorCode::Overflow) });
    }

    if (required > capacity_)
    {
        SizeType newCapacity = NextCapacity();

        while (newCapacity < required)
        {
            newCapacity = GrowCapacity(newCapacity, kStringMaxCapacity);
        }

        if (Result<void> result = Reserve(newCapacity); result.HasError())
        {
            return result;
        }
    }

    std::memcpy(data_ + size_, text.Data(), text.Size());
    size_ += text.Size();
    data_[size_] = '\0';

    return {};
}

inline void String::Swap(
    String& other) noexcept
{
    using std::swap;

    swap(data_, other.data_);
    swap(size_, other.size_);
    swap(capacity_, other.capacity_);
    swap(allocator_, other.allocator_);
}

//==============================================================================
// Construction (copy/move — placed after Modifiers so Append is visible)
//==============================================================================

inline String::String(
    const String& other)
    :
    String(*other.allocator_)
{
    if (other.capacity_ > 0)
    {
        Reserve(other.capacity_).Ignore();
    }

    // If the Reserve() above failed to allocate (out of memory), capacity_
    // stays at 0 and data_ stays nullptr. Bound the copy to whatever
    // capacity was actually secured instead of writing past an empty
    // buffer — same precedent as Vector<T>'s copy constructor (see
    // HISTORY.md).
    const SizeType count = capacity_ < other.size_ ? capacity_ : other.size_;

    if (count > 0)
    {
        std::memcpy(data_, other.data_, count);
    }

    size_ = count;

    if (data_ != nullptr)
    {
        data_[size_] = '\0';
    }
}

inline String::String(
    String&& other) noexcept
    :
    data_(other.data_),
    size_(other.size_),
    capacity_(other.capacity_),
    allocator_(other.allocator_)
{
    other.data_ = nullptr;
    other.size_ = 0;
    other.capacity_ = 0;
}

//==============================================================================
// Assignment
//==============================================================================

inline String& String::operator=(
    const String& other)
{
    if (this == &other)
    {
        return *this;
    }

    Clear();

    if (capacity_ < other.size_)
    {
        Reserve(other.capacity_).Ignore();
    }

    // Same reasoning as the copy constructor: don't trust that Reserve()
    // above actually grew capacity_ far enough before copying into it.
    const SizeType count = capacity_ < other.size_ ? capacity_ : other.size_;

    if (count > 0)
    {
        std::memcpy(data_, other.data_, count);
    }

    size_ = count;

    if (data_ != nullptr)
    {
        data_[size_] = '\0';
    }

    return *this;
}

inline String& String::operator=(
    String&& other) noexcept
{
    if (this == &other)
    {
        return *this;
    }

    Clear();

    if (data_ != nullptr)
    {
        allocator_->Deallocate(
            data_,
            AllocationSize(capacity_),
            alignof(char));
    }

    data_ = other.data_;
    size_ = other.size_;
    capacity_ = other.capacity_;
    allocator_ = other.allocator_;

    other.data_ = nullptr;
    other.size_ = 0;
    other.capacity_ = 0;

    return *this;
}

} // namespace forge::core
// NOTE: everything above this line must stay inside namespace forge::core —
// see the identical note in Vector.inl/Result.inl for why this matters.
