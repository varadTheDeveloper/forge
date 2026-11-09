#pragma once

#include <memory>
#include <utility>

#include "Assert.h"
#include "CheckedMath.h"
#include "Construct.h"

namespace forge::core
{

//==============================================================================
// Construction
//==============================================================================

template<typename K, typename V>
HashMap<K, V>::HashMap() noexcept
    :
    states_(nullptr),
    keys_(nullptr),
    values_(nullptr),
    capacity_(0),
    size_(0),
    tombstones_(0),
    allocator_(&memory::GetDefaultAllocator())
{
}

template<typename K, typename V>
HashMap<K, V>::HashMap(
    memory::Allocator& allocator) noexcept
    :
    states_(nullptr),
    keys_(nullptr),
    values_(nullptr),
    capacity_(0),
    size_(0),
    tombstones_(0),
    allocator_(&allocator)
{
}

template<typename K, typename V>
HashMap<K, V>::~HashMap() noexcept
{
    Clear();

    if (states_ != nullptr)
    {
        allocator_->Deallocate(states_, capacity_ * sizeof(SlotState), alignof(SlotState));
    }

    if (keys_ != nullptr)
    {
        allocator_->Deallocate(keys_, capacity_ * sizeof(K), alignof(K));
    }

    if (values_ != nullptr)
    {
        allocator_->Deallocate(values_, capacity_ * sizeof(V), alignof(V));
    }
}

//==============================================================================
// Capacity
//==============================================================================

template<typename K, typename V>
typename HashMap<K, V>::SizeType
HashMap<K, V>::Size() const noexcept
{
    return size_;
}

template<typename K, typename V>
typename HashMap<K, V>::SizeType
HashMap<K, V>::Capacity() const noexcept
{
    return capacity_;
}

template<typename K, typename V>
bool HashMap<K, V>::Empty() const noexcept
{
    return size_ == 0;
}

template<typename K, typename V>
constexpr typename HashMap<K, V>::SizeType
HashMap<K, V>::MaxCapacity() noexcept
{
    SizeType limit = MaxElementCount<K>();

    if (MaxElementCount<V>() < limit)
    {
        limit = MaxElementCount<V>();
    }

    if (MaxElementCount<SlotState>() < limit)
    {
        limit = MaxElementCount<SlotState>();
    }

    SizeType capacity = MinCapacity();

    while (capacity <= limit / 2)
    {
        capacity *= 2;
    }

    return capacity;
}

template<typename K, typename V>
typename HashMap<K, V>::SizeType
HashMap<K, V>::NextCapacity(
    SizeType atLeast) noexcept
{
    // Returns 0 when no allowed capacity is large enough; callers treat
    // that as an overflow error.
    if (atLeast > MaxCapacity())
    {
        return 0;
    }

    SizeType capacity = MinCapacity();

    while (capacity < atLeast)
    {
        capacity *= 2;
    }

    return capacity;
}

template<typename K, typename V>
bool HashMap<K, V>::NeedsGrowth() const noexcept
{
    if (capacity_ == 0)
    {
        return true;
    }

    // Keep load factor (including tombstones, which cost probe length
    // just as much as real entries do) under 75%.
    // used > 3/4 of capacity_, written so it cannot overflow (capacity_
    // is a power of two >= MinCapacity(), so this is exact).
    const SizeType used = size_ + tombstones_ + 1;
    return used > capacity_ - capacity_ / 4;
}

template<typename K, typename V>
Result<void> HashMap<K, V>::GrowIfNeeded()
{
    if (!NeedsGrowth())
    {
        return {};
    }

    if (capacity_ >= MaxCapacity())
    {
        return Result<void>(Failure{ Error(ErrorCode::Overflow) });
    }

    return RehashTo(
        capacity_ == 0 ? MinCapacity() : capacity_ * 2);
}

template<typename K, typename V>
Result<void> HashMap<K, V>::Reserve(
    SizeType minSize)
{
    if (minSize == 0)
    {
        return {};
    }

    // ceil(minSize / 0.75) == ceil(minSize * 4 / 3).
    SizeType scaled = 0;

    if (!CheckedMul(minSize, 4, &scaled) || !CheckedAdd(scaled, 2, &scaled))
    {
        return Result<void>(Failure{ Error(ErrorCode::Overflow) });
    }

    const SizeType required = scaled / 3;
    const SizeType newCapacity = NextCapacity(required);

    if (newCapacity == 0)
    {
        return Result<void>(Failure{ Error(ErrorCode::Overflow) });
    }

    if (newCapacity <= capacity_)
    {
        return {};
    }

    return RehashTo(newCapacity);
}

template<typename K, typename V>
Result<void> HashMap<K, V>::RehashTo(
    SizeType newCapacity)
{
    // Every caller computes newCapacity through NextCapacity() or a
    // MaxCapacity()-checked doubling, so the byte sizes below fit.
    FORGE_RELEASE_ASSERT(newCapacity >= MinCapacity() && newCapacity <= MaxCapacity());
    void* statesMemory = allocator_->Allocate(
        newCapacity * sizeof(SlotState),
        alignof(SlotState));

    if (statesMemory == nullptr)
    {
        return Result<void>(Failure{ Error(ErrorCode::OutOfMemory) });
    }

    void* keysMemory = allocator_->Allocate(
        newCapacity * sizeof(K),
        alignof(K));

    if (keysMemory == nullptr)
    {
        allocator_->Deallocate(statesMemory, newCapacity * sizeof(SlotState), alignof(SlotState));
        return Result<void>(Failure{ Error(ErrorCode::OutOfMemory) });
    }

    void* valuesMemory = allocator_->Allocate(
        newCapacity * sizeof(V),
        alignof(V));

    if (valuesMemory == nullptr)
    {
        allocator_->Deallocate(keysMemory, newCapacity * sizeof(K), alignof(K));
        allocator_->Deallocate(statesMemory, newCapacity * sizeof(SlotState), alignof(SlotState));
        return Result<void>(Failure{ Error(ErrorCode::OutOfMemory) });
    }

    SlotState* newStates = static_cast<SlotState*>(statesMemory);
    K* newKeys = static_cast<K*>(keysMemory);
    V* newValues = static_cast<V*>(valuesMemory);

    for (SizeType index = 0; index < newCapacity; ++index)
    {
        newStates[index] = SlotState::Empty;
    }

    // Re-probe every occupied slot from the old table into the new one.
    // Tombstones are simply dropped — that's the whole point of a
    // rehash, and is why tombstones_ resets to 0 below.
    for (SizeType oldIndex = 0; oldIndex < capacity_; ++oldIndex)
    {
        if (states_[oldIndex] != SlotState::Occupied)
        {
            continue;
        }

        const u64 hash = Hash<K>{}(keys_[oldIndex]);
        SizeType index = static_cast<SizeType>(hash) & (newCapacity - 1);

        while (newStates[index] == SlotState::Occupied)
        {
            index = (index + 1) & (newCapacity - 1);
        }

        newStates[index] = SlotState::Occupied;

        forge::core::detail::ConstructAt(
            newKeys + index,
            std::move_if_noexcept(keys_[oldIndex]));

        forge::core::detail::ConstructAt(
            newValues + index,
            std::move_if_noexcept(values_[oldIndex]));
    }

    for (SizeType oldIndex = 0; oldIndex < capacity_; ++oldIndex)
    {
        if (states_[oldIndex] == SlotState::Occupied)
        {
            std::destroy_at(keys_ + oldIndex);
            std::destroy_at(values_ + oldIndex);
        }
    }

    if (states_ != nullptr)
    {
        allocator_->Deallocate(states_, capacity_ * sizeof(SlotState), alignof(SlotState));
    }

    if (keys_ != nullptr)
    {
        allocator_->Deallocate(keys_, capacity_ * sizeof(K), alignof(K));
    }

    if (values_ != nullptr)
    {
        allocator_->Deallocate(values_, capacity_ * sizeof(V), alignof(V));
    }

    states_ = newStates;
    keys_ = newKeys;
    values_ = newValues;
    capacity_ = newCapacity;
    tombstones_ = 0;

    return {};
}

template<typename K, typename V>
void HashMap<K, V>::Clear() noexcept
{
    for (SizeType index = 0; index < capacity_; ++index)
    {
        if (states_[index] == SlotState::Occupied)
        {
            std::destroy_at(keys_ + index);
            std::destroy_at(values_ + index);
        }

        states_[index] = SlotState::Empty;
    }

    size_ = 0;
    tombstones_ = 0;
}

//==============================================================================
// Probing
//==============================================================================

template<typename K, typename V>
typename HashMap<K, V>::SizeType
HashMap<K, V>::ProbeForInsert(
    const K& key,
    bool& outFound) const noexcept
{
    const u64 hash = Hash<K>{}(key);
    SizeType index = static_cast<SizeType>(hash) & (capacity_ - 1);
    SizeType firstTombstone = capacity_; // capacity_ == "none found yet"

    for (SizeType probes = 0; probes < capacity_; ++probes)
    {
        const SlotState state = states_[index];

        if (state == SlotState::Empty)
        {
            outFound = false;
            return firstTombstone != capacity_ ? firstTombstone : index;
        }

        if (state == SlotState::Occupied && keys_[index] == key)
        {
            outFound = true;
            return index;
        }

        if (state == SlotState::Tombstone && firstTombstone == capacity_)
        {
            firstTombstone = index;
        }

        index = (index + 1) & (capacity_ - 1);
    }

    // Unreachable in practice: GrowIfNeeded() keeps size_ + tombstones_
    // well under capacity_, so an Empty slot always exists along the
    // probe sequence. Kept as a safe fallback rather than an infinite
    // loop / out-of-bounds return if that invariant is ever violated.
    outFound = false;
    return firstTombstone != capacity_ ? firstTombstone : 0;
}

template<typename K, typename V>
typename HashMap<K, V>::SizeType
HashMap<K, V>::ProbeForFind(
    const K& key) const noexcept
{
    // capacity_ == 0 (never allocated) falls out of this correctly
    // without dereferencing states_/keys_: the loop below runs zero
    // times when capacity_ is 0, so it falls straight through to
    // `return capacity_;` (0), which every caller already treats as the
    // "not found" sentinel.
    const u64 hash = Hash<K>{}(key);
    SizeType index = static_cast<SizeType>(hash) & (capacity_ - 1);

    for (SizeType probes = 0; probes < capacity_; ++probes)
    {
        const SlotState state = states_[index];

        if (state == SlotState::Empty)
        {
            return capacity_;
        }

        if (state == SlotState::Occupied && keys_[index] == key)
        {
            return index;
        }

        // Tombstone: the key may still be further along the probe
        // sequence, unlike ProbeForInsert this never stops the search.
        index = (index + 1) & (capacity_ - 1);
    }

    return capacity_;
}

//==============================================================================
// Lookup
//==============================================================================

template<typename K, typename V>
bool HashMap<K, V>::Contains(
    const K& key) const noexcept
{
    return ProbeForFind(key) != capacity_;
}

template<typename K, typename V>
V* HashMap<K, V>::Find(
    const K& key) noexcept
{
    const SizeType index = ProbeForFind(key);
    return index != capacity_ ? &values_[index] : nullptr;
}

template<typename K, typename V>
const V* HashMap<K, V>::Find(
    const K& key) const noexcept
{
    const SizeType index = ProbeForFind(key);
    return index != capacity_ ? &values_[index] : nullptr;
}

//==============================================================================
// Modifiers
//==============================================================================

template<typename K, typename V>
Result<bool> HashMap<K, V>::Insert(
    const K& key,
    const V& value)
{
    if (Result<void> grown = GrowIfNeeded(); grown.HasError())
    {
        return Result<bool>(Failure{ grown.Error() });
    }

    bool found = false;
    const SizeType index = ProbeForInsert(key, found);

    if (found)
    {
        values_[index] = value;
        return Result<bool>(false);
    }

    const bool wasTombstone = (states_[index] == SlotState::Tombstone);

    forge::core::detail::ConstructAt(keys_ + index, key);
    forge::core::detail::ConstructAt(values_ + index, value);
    states_[index] = SlotState::Occupied;

    ++size_;

    if (wasTombstone)
    {
        --tombstones_;
    }

    return Result<bool>(true);
}

template<typename K, typename V>
Result<bool> HashMap<K, V>::Insert(
    const K& key,
    V&& value)
{
    if (Result<void> grown = GrowIfNeeded(); grown.HasError())
    {
        return Result<bool>(Failure{ grown.Error() });
    }

    bool found = false;
    const SizeType index = ProbeForInsert(key, found);

    if (found)
    {
        values_[index] = std::move(value);
        return Result<bool>(false);
    }

    const bool wasTombstone = (states_[index] == SlotState::Tombstone);

    forge::core::detail::ConstructAt(keys_ + index, key);
    forge::core::detail::ConstructAt(values_ + index, std::move(value));
    states_[index] = SlotState::Occupied;

    ++size_;

    if (wasTombstone)
    {
        --tombstones_;
    }

    return Result<bool>(true);
}

template<typename K, typename V>
bool HashMap<K, V>::Erase(
    const K& key) noexcept
{
    const SizeType index = ProbeForFind(key);

    if (index == capacity_)
    {
        return false;
    }

    std::destroy_at(keys_ + index);
    std::destroy_at(values_ + index);
    states_[index] = SlotState::Tombstone;

    --size_;
    ++tombstones_;

    return true;
}

template<typename K, typename V>
void HashMap<K, V>::Swap(
    HashMap& other) noexcept
{
    using std::swap;

    swap(states_, other.states_);
    swap(keys_, other.keys_);
    swap(values_, other.values_);
    swap(capacity_, other.capacity_);
    swap(size_, other.size_);
    swap(tombstones_, other.tombstones_);
    swap(allocator_, other.allocator_);
}

//==============================================================================
// Iteration
//==============================================================================

template<typename K, typename V>
typename HashMap<K, V>::Iterator
HashMap<K, V>::begin() noexcept
{
    Iterator iterator(this, 0);
    iterator.SkipToOccupied();
    return iterator;
}

template<typename K, typename V>
typename HashMap<K, V>::Iterator
HashMap<K, V>::end() noexcept
{
    return Iterator(this, capacity_);
}

//==============================================================================
// Construction (copy/move — placed after Modifiers so Insert is visible)
//==============================================================================

template<typename K, typename V>
HashMap<K, V>::HashMap(
    const HashMap& other)
    :
    HashMap(*other.allocator_)
{
    // Best-effort: if this fails (or only partially succeeds), the
    // Insert() loop below simply starts failing once the (smaller) table
    // it secured fills up, and we stop copying further entries — same
    // truncate-safely-on-OOM outcome as Vector<T>/String's copy
    // constructor, just reached by composing Insert() (which already
    // knows how to grow/fail safely) rather than a raw indexed copy loop.
    Reserve(other.size_).Ignore();

    for (SizeType index = 0; index < other.capacity_; ++index)
    {
        if (other.states_[index] == SlotState::Occupied)
        {
            if (Insert(other.keys_[index], other.values_[index]).HasError())
            {
                break;
            }
        }
    }
}

template<typename K, typename V>
HashMap<K, V>::HashMap(
    HashMap&& other) noexcept
    :
    states_(other.states_),
    keys_(other.keys_),
    values_(other.values_),
    capacity_(other.capacity_),
    size_(other.size_),
    tombstones_(other.tombstones_),
    allocator_(other.allocator_)
{
    other.states_ = nullptr;
    other.keys_ = nullptr;
    other.values_ = nullptr;
    other.capacity_ = 0;
    other.size_ = 0;
    other.tombstones_ = 0;
}

//==============================================================================
// Assignment
//==============================================================================

template<typename K, typename V>
HashMap<K, V>& HashMap<K, V>::operator=(
    const HashMap& other)
{
    if (this == &other)
    {
        return *this;
    }

    Clear();

    for (SizeType index = 0; index < other.capacity_; ++index)
    {
        if (other.states_[index] == SlotState::Occupied)
        {
            if (Insert(other.keys_[index], other.values_[index]).HasError())
            {
                break;
            }
        }
    }

    return *this;
}

template<typename K, typename V>
HashMap<K, V>& HashMap<K, V>::operator=(
    HashMap&& other) noexcept
{
    if (this == &other)
    {
        return *this;
    }

    Clear();

    if (states_ != nullptr)
    {
        allocator_->Deallocate(states_, capacity_ * sizeof(SlotState), alignof(SlotState));
    }

    if (keys_ != nullptr)
    {
        allocator_->Deallocate(keys_, capacity_ * sizeof(K), alignof(K));
    }

    if (values_ != nullptr)
    {
        allocator_->Deallocate(values_, capacity_ * sizeof(V), alignof(V));
    }

    states_ = other.states_;
    keys_ = other.keys_;
    values_ = other.values_;
    capacity_ = other.capacity_;
    size_ = other.size_;
    tombstones_ = other.tombstones_;
    allocator_ = other.allocator_;

    other.states_ = nullptr;
    other.keys_ = nullptr;
    other.values_ = nullptr;
    other.capacity_ = 0;
    other.size_ = 0;
    other.tombstones_ = 0;

    return *this;
}

} // namespace forge::core
