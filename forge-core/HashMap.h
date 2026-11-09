#pragma once

#include "Hash.h"
#include "memory/ResultVoid.h"
#include "memory/Allocator.h"
#include "memory/DefaultAllocator.h"
#include "Types.h"

namespace forge::core::detail
{

/// Empty placeholder value type. HashSet<K> is implemented as a thin
/// wrapper around HashMap<K, Unit> (see HashSet.h) rather than a second,
/// independent hash-table implementation — Unit is what stands in for
/// "no value" so HashMap's machinery (which always stores a K and a V
/// per slot) can be reused as-is.
struct Unit
{
};

} // namespace forge::core::detail

namespace forge::core
{

/// Open-addressing hash table, keyed by `Hash<K>` (see Hash.h — a key
/// type must have an explicit Hash<K> specialization; there is no
/// fallback to hashing raw bytes). Linear probing with tombstones for
/// deletion, matching the classic, well-understood design rather than
/// something more exotic (Robin Hood / SwissTable-style SIMD probing) —
/// correct and simple beats clever here, per PROJECT_CONTEXT.md's own
/// "Readability over cleverness".
///
/// Does not support a custom hasher/key-equality template parameter, or
/// const iteration, yet — both are straightforward to add later if a
/// concrete use case needs them; left out for now to keep the first
/// version's surface area small.
///
/// Same Allocator/Result<void>/copy-truncates-safely-on-OOM conventions
/// as Vector<T>/String throughout.
template<typename K, typename V>
class [[nodiscard]] HashMap
{
private:

    enum class SlotState : u8
    {
        Empty,
        Occupied,
        Tombstone,
    };

public:

    using SizeType = forge::core::Size;

    /// A reference-like view of one occupied slot, handed out by
    /// Iterator's operator*(). Does not own the key/value — it aliases
    /// storage inside the HashMap, same lifetime rules as a Vector<T>
    /// element reference.
    class Entry
    {
    public:

        Entry(
            const K& key,
            V& value) noexcept
            :
            key_(key),
            value_(value)
        {
        }

        [[nodiscard]]
        const K& Key() const noexcept
        {
            return key_;
        }

        [[nodiscard]]
        V& Value() const noexcept
        {
            return value_;
        }

    private:

        const K& key_;
        V& value_;
    };

    /// Forward-only iterator over occupied slots. Invalidated by any
    /// mutating call (Insert/Erase/Reserve/Clear) exactly like Vector<T>
    /// iterators/pointers are invalidated by Reserve.
    class Iterator
    {
    public:

        Iterator& operator++() noexcept
        {
            ++index_;
            SkipToOccupied();
            return *this;
        }

        [[nodiscard]]
        Entry operator*() const noexcept
        {
            return Entry(table_->keys_[index_], table_->values_[index_]);
        }

        [[nodiscard]]
        bool operator==(
            const Iterator& other) const noexcept
        {
            return table_ == other.table_ && index_ == other.index_;
        }

        [[nodiscard]]
        bool operator!=(
            const Iterator& other) const noexcept
        {
            return !(*this == other);
        }

    private:

        friend class HashMap;

        Iterator(
            HashMap* table,
            SizeType index) noexcept
            :
            table_(table),
            index_(index)
        {
        }

        void SkipToOccupied() noexcept
        {
            while (index_ < table_->capacity_ &&
                   table_->states_[index_] != SlotState::Occupied)
            {
                ++index_;
            }
        }

        HashMap* table_;
        SizeType index_;
    };

    //==========================================================================
    // Construction
    //==========================================================================

    HashMap() noexcept;

    explicit HashMap(
        memory::Allocator& allocator) noexcept;

    HashMap(
        const HashMap& other);

    HashMap(
        HashMap&& other) noexcept;

    ~HashMap() noexcept;

    //==========================================================================
    // Assignment
    //==========================================================================

    HashMap& operator=(
        const HashMap& other);

    HashMap& operator=(
        HashMap&& other) noexcept;

    //==========================================================================
    // Capacity
    //==========================================================================

    [[nodiscard]]
    SizeType Size() const noexcept;

    [[nodiscard]]
    SizeType Capacity() const noexcept;

    [[nodiscard]]
    bool Empty() const noexcept;

    /// Ensures the table can hold at least `minSize` entries without
    /// needing to grow again. Rounds up to the table's own power-of-two/
    /// load-factor rules — unlike Vector<T>::Reserve, the resulting
    /// Capacity() is not guaranteed to equal `minSize` exactly.
    Result<void> Reserve(
        SizeType minSize);

    void Clear() noexcept;

    //==========================================================================
    // Lookup
    //==========================================================================

    [[nodiscard]]
    bool Contains(
        const K& key) const noexcept;

    [[nodiscard]]
    V* Find(
        const K& key) noexcept;

    [[nodiscard]]
    const V* Find(
        const K& key) const noexcept;

    //==========================================================================
    // Modifiers
    //==========================================================================

    /// Inserts `key`/`value`, or replaces the existing value if `key` is
    /// already present. On success, the bool is true if this was a new
    /// key, false if an existing entry's value was replaced.
    Result<bool> Insert(
        const K& key,
        const V& value);

    Result<bool> Insert(
        const K& key,
        V&& value);

    /// Removes `key` if present. Returns true if something was removed.
    bool Erase(
        const K& key) noexcept;

    void Swap(
        HashMap& other) noexcept;

    //==========================================================================
    // Iteration
    //==========================================================================

    [[nodiscard]]
    Iterator begin() noexcept;

    [[nodiscard]]
    Iterator end() noexcept;

private:

    [[nodiscard]]
    static constexpr SizeType MinCapacity() noexcept
    {
        return 8;
    }

    /// Largest power-of-two slot count whose keys, values and slot-state
    /// arrays all have byte sizes that fit (Phase 7.9: capacity math used
    /// to wrap silently).
    [[nodiscard]]
    static constexpr SizeType MaxCapacity() noexcept;

    /// Slot count grows in powers of two so the table index can be
    /// computed with `hash & (capacity_ - 1)` instead of a modulo.
    [[nodiscard]]
    static SizeType NextCapacity(
        SizeType atLeast) noexcept;

    /// True once (size_ + tombstones_) crosses 75% of capacity_ — counting
    /// tombstones here (not just size_) is what actually bounds probe
    /// length; a table full of tombstones is just as slow to probe as one
    /// full of real entries.
    [[nodiscard]]
    bool NeedsGrowth() const noexcept;

    Result<void> GrowIfNeeded();

    Result<void> RehashTo(
        SizeType newCapacity);

    /// Returns the physical slot for `key`: an Occupied slot with an
    /// equal key if one exists, otherwise the first Empty-or-Tombstone
    /// slot found along the probe sequence, which is where `key` should
    /// be inserted. `outFound` reports which case it was.
    [[nodiscard]]
    SizeType ProbeForInsert(
        const K& key,
        bool& outFound) const noexcept;

    /// Returns the physical slot of an Occupied entry equal to `key`, or
    /// capacity_ (an always-invalid index) if not present. Unlike
    /// ProbeForInsert, a Tombstone never stops the search — the key may
    /// still be further along the probe sequence.
    [[nodiscard]]
    SizeType ProbeForFind(
        const K& key) const noexcept;

private:

    SlotState* states_{ nullptr };

    K* keys_{ nullptr };

    V* values_{ nullptr };

    SizeType capacity_{ 0 };

    SizeType size_{ 0 };

    SizeType tombstones_{ 0 };

    memory::Allocator* allocator_{ &memory::GetDefaultAllocator() };
};

} // namespace forge::core

#include "HashMap.inl"
