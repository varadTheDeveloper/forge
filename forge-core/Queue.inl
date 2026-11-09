#pragma once

#include <memory>
#include <utility>

#include "Assert.h"
#include "CheckedMath.h"
#include "Construct.h"

namespace forge::core
{

//==============================================================================
// Helpers
//==============================================================================

template<typename T>
constexpr typename Queue<T>::SizeType
Queue<T>::AllocationSize(
    SizeType capacity) noexcept
{
    return capacity * sizeof(T);
}

template<typename T>
typename Queue<T>::SizeType
Queue<T>::NextCapacity() const noexcept
{
    // Saturates at MaxElementCount<T>() instead of wrapping (Phase 7.9).
    return GrowCapacity(capacity_, MaxElementCount<T>());
}

template<typename T>
typename Queue<T>::SizeType
Queue<T>::PhysicalIndex(
    SizeType offset) const noexcept
{
    return (head_ + offset) % capacity_;
}

//==============================================================================
// Construction
//==============================================================================

template<typename T>
Queue<T>::Queue() noexcept
    :
    data_(nullptr),
    size_(0),
    capacity_(0),
    head_(0),
    allocator_(&memory::GetDefaultAllocator())
{
}

template<typename T>
Queue<T>::Queue(
    memory::Allocator& allocator) noexcept
    :
    data_(nullptr),
    size_(0),
    capacity_(0),
    head_(0),
    allocator_(&allocator)
{
}

template<typename T>
Queue<T>::~Queue() noexcept
{
    Clear();

    if (data_ != nullptr)
    {
        allocator_->Deallocate(
            data_,
            AllocationSize(capacity_),
            alignof(T));
    }
}

//==============================================================================
// Capacity
//==============================================================================

template<typename T>
typename Queue<T>::SizeType
Queue<T>::Size() const noexcept
{
    return size_;
}

template<typename T>
typename Queue<T>::SizeType
Queue<T>::Capacity() const noexcept
{
    return capacity_;
}

template<typename T>
bool Queue<T>::Empty() const noexcept
{
    return size_ == 0;
}

template<typename T>
Result<void> Queue<T>::Reserve(
    SizeType capacity)
{
    if (capacity <= capacity_)
    {
        return {};
    }

    // Phase 7.9: reject capacities whose byte size would overflow.
    if (capacity > MaxElementCount<T>())
    {
        return Result<void>(
            Failure{ Error(ErrorCode::Overflow) });
    }

    void* memory = allocator_->Allocate(
        AllocationSize(capacity),
        alignof(T));

    if (memory == nullptr)
    {
        return Result<void>(
            Failure{ Error(ErrorCode::OutOfMemory) });
    }

    T* newData = static_cast<T*>(memory);

    // Linearize: element 0 of the logical order always lands at index 0
    // of the new buffer, regardless of where `head_` was pointing in the
    // old one — the new buffer starts with no wraparound.
    for (SizeType index = 0; index < size_; ++index)
    {
        forge::core::detail::ConstructAt(
            newData + index,
            std::move_if_noexcept(data_[PhysicalIndex(index)]));
    }

    for (SizeType index = 0; index < size_; ++index)
    {
        std::destroy_at(data_ + PhysicalIndex(index));
    }

    if (data_ != nullptr)
    {
        allocator_->Deallocate(
            data_,
            AllocationSize(capacity_),
            alignof(T));
    }

    data_ = newData;
    capacity_ = capacity;
    head_ = 0;

    return {};
}

template<typename T>
Result<void> Queue<T>::GrowIfNeeded()
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

template<typename T>
void Queue<T>::Clear() noexcept
{
    for (SizeType index = 0; index < size_; ++index)
    {
        std::destroy_at(data_ + PhysicalIndex(index));
    }

    size_ = 0;
    head_ = 0;
}

//==============================================================================
// Element Access
//==============================================================================

template<typename T>
T& Queue<T>::Front() noexcept
{
    FORGE_RELEASE_ASSERT(size_ > 0);
    return data_[head_];
}

template<typename T>
const T& Queue<T>::Front() const noexcept
{
    FORGE_RELEASE_ASSERT(size_ > 0);
    return data_[head_];
}

template<typename T>
T& Queue<T>::Back() noexcept
{
    FORGE_RELEASE_ASSERT(size_ > 0);
    return data_[PhysicalIndex(size_ - 1)];
}

template<typename T>
const T& Queue<T>::Back() const noexcept
{
    FORGE_RELEASE_ASSERT(size_ > 0);
    return data_[PhysicalIndex(size_ - 1)];
}

template<typename T>
T& Queue<T>::operator[](
    SizeType offset) noexcept
{
    FORGE_RELEASE_ASSERT(offset < size_);
    return data_[PhysicalIndex(offset)];
}

template<typename T>
const T& Queue<T>::operator[](
    SizeType offset) const noexcept
{
    FORGE_RELEASE_ASSERT(offset < size_);
    return data_[PhysicalIndex(offset)];
}

//==============================================================================
// Modifiers
//==============================================================================

template<typename T>
Result<void> Queue<T>::Push(
    const T& value)
{
    if (Result<void> result = GrowIfNeeded(); result.HasError())
    {
        return result;
    }

    forge::core::detail::ConstructAt(
        data_ + PhysicalIndex(size_),
        value);

    ++size_;

    return {};
}

template<typename T>
Result<void> Queue<T>::Push(
    T&& value)
{
    if (Result<void> result = GrowIfNeeded(); result.HasError())
    {
        return result;
    }

    forge::core::detail::ConstructAt(
        data_ + PhysicalIndex(size_),
        std::move(value));

    ++size_;

    return {};
}

template<typename T>
template<typename... Args>
Result<void> Queue<T>::Emplace(
    Args&&... args)
{
    if (Result<void> result = GrowIfNeeded(); result.HasError())
    {
        return result;
    }

    forge::core::detail::ConstructAt(
        data_ + PhysicalIndex(size_),
        std::forward<Args>(args)...);

    ++size_;

    return {};
}

template<typename T>
void Queue<T>::Pop() noexcept
{
    if (size_ == 0)
    {
        return;
    }

    std::destroy_at(data_ + head_);
    head_ = (head_ + 1) % capacity_;
    --size_;
}

template<typename T>
void Queue<T>::Swap(
    Queue& other) noexcept
{
    using std::swap;

    swap(data_, other.data_);
    swap(size_, other.size_);
    swap(capacity_, other.capacity_);
    swap(head_, other.head_);
    swap(allocator_, other.allocator_);
}

//==============================================================================
// Construction (copy/move — placed after Modifiers so Reserve is visible)
//==============================================================================

template<typename T>
Queue<T>::Queue(
    const Queue& other)
    :
    Queue(*other.allocator_)
{
    if (other.capacity_ > 0)
    {
        Reserve(other.capacity_).Ignore();
    }

    // If Reserve() above couldn't secure other's full capacity (OOM),
    // bound the copy to whatever capacity was actually secured — same
    // truncate-safely-on-OOM precedent as Vector<T>/String.
    const SizeType count = capacity_ < other.size_ ? capacity_ : other.size_;

    for (SizeType index = 0; index < count; ++index)
    {
        forge::core::detail::ConstructAt(
            data_ + index,
            other.data_[other.PhysicalIndex(index)]);
    }

    size_ = count;
    head_ = 0;
}

template<typename T>
Queue<T>::Queue(
    Queue&& other) noexcept
    :
    data_(other.data_),
    size_(other.size_),
    capacity_(other.capacity_),
    head_(other.head_),
    allocator_(other.allocator_)
{
    other.data_ = nullptr;
    other.size_ = 0;
    other.capacity_ = 0;
    other.head_ = 0;
}

//==============================================================================
// Assignment
//==============================================================================

template<typename T>
Queue<T>& Queue<T>::operator=(
    const Queue& other)
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

    const SizeType count = capacity_ < other.size_ ? capacity_ : other.size_;

    for (SizeType index = 0; index < count; ++index)
    {
        forge::core::detail::ConstructAt(
            data_ + index,
            other.data_[other.PhysicalIndex(index)]);
    }

    size_ = count;
    head_ = 0;

    return *this;
}

template<typename T>
Queue<T>& Queue<T>::operator=(
    Queue&& other) noexcept
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
            alignof(T));
    }

    data_ = other.data_;
    size_ = other.size_;
    capacity_ = other.capacity_;
    head_ = other.head_;
    allocator_ = other.allocator_;

    other.data_ = nullptr;
    other.size_ = 0;
    other.capacity_ = 0;
    other.head_ = 0;

    return *this;
}

} // namespace forge::core
