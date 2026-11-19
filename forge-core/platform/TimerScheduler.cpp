#include "TimerScheduler.h"

#include <utility>

namespace forge::core::platform
{

Size TimerScheduler::IndexOf(TimerId id) const noexcept
{
    for (Size index = 0; index < entries_.Size(); ++index)
    {
        if (entries_[index].id == id)
        {
            return index;
        }
    }

    return entries_.Size(); // sentinel: "not found"
}

Result<TimerId> TimerScheduler::Schedule(
    u64 nowMs,
    u64 delayMs,
    bool repeat,
    TimerCallback callback,
    void* userData) noexcept
{
    Entry entry;
    entry.id = nextId_;
    entry.dueMs = nowMs + delayMs;
    entry.delayMs = delayMs;
    entry.repeat = repeat;
    entry.cancelled = false;
    entry.callback = callback;
    entry.userData = userData;

    if (Result<void> result = entries_.PushBack(entry); result.HasError())
    {
        return Result<TimerId>(Failure(result.Error()));
    }

    return Result<TimerId>(TimerId(nextId_++));
}

void TimerScheduler::Cancel(TimerId id) noexcept
{
    Size index = IndexOf(id);

    if (index == entries_.Size())
    {
        return; // not found: already fired-and-removed, or never existed
    }

    entries_[index].cancelled = true;
}

bool TimerScheduler::Empty() const noexcept
{
    return Count() == 0;
}

Size TimerScheduler::Count() const noexcept
{
    Size count = 0;

    for (Size index = 0; index < entries_.Size(); ++index)
    {
        if (!entries_[index].cancelled)
        {
            ++count;
        }
    }

    return count;
}

bool TimerScheduler::NextDueDelay(u64 nowMs, u64& outDelayMs) const noexcept
{
    bool found = false;
    u64 earliest = 0;

    for (Size index = 0; index < entries_.Size(); ++index)
    {
        if (entries_[index].cancelled)
        {
            continue;
        }

        if (!found || entries_[index].dueMs < earliest)
        {
            earliest = entries_[index].dueMs;
            found = true;
        }
    }

    if (!found)
    {
        return false;
    }

    outDelayMs = (earliest > nowMs) ? (earliest - nowMs) : 0;
    return true;
}

Size TimerScheduler::PopDue(u64 nowMs) noexcept
{
    // Due timers fire in order of due time, and timers due at the same time
    // in the order they were scheduled (lower id first), as HTML and Node
    // order setTimeout callbacks. (Before Phase 10.3 they fired in vector
    // order, which swap-removal scrambled: three timers due together could
    // fire in reverse.) Each pass picks the earliest due timer afresh, so a
    // timer scheduled or cancelled by a callback is seen correctly; a
    // repeating timer fires at most once per PopDue call.
    const u64 sweep = ++sweep_;
    Size firedCount = 0;

    for (;;)
    {
        bool found = false;
        Size best = 0;
        Size index = 0;

        while (index < entries_.Size())
        {
            if (entries_[index].cancelled)
            {
                // Swap-removal only moves an entry not visited yet into
                // `index`, so `best` (an index below it) stays valid.
                entries_[index] = std::move(entries_[entries_.Size() - 1]);
                entries_.PopBack();
                continue; // re-check this index: it now holds a different entry
            }

            const Entry& entry = entries_[index];

            if (entry.dueMs <= nowMs && entry.firedInSweep != sweep &&
                (!found || entry.dueMs < entries_[best].dueMs ||
                 (entry.dueMs == entries_[best].dueMs && entry.id < entries_[best].id)))
            {
                found = true;
                best = index;
            }

            ++index;
        }

        if (!found)
        {
            return firedCount;
        }

        // Copy out before touching the vector: Schedule()/Cancel() called
        // from inside the callback below may grow or shrink entries_, and
        // we must not hold a dangling reference across that call.
        Entry fired = entries_[best];

        if (fired.repeat)
        {
            // Anchor to the timer's own previous due time, not to `nowMs`
            // (when PopDue happened to be called). Anchoring to `nowMs`
            // would let the effective period drift longer under repeated
            // late polling; anchoring to `fired.dueMs` keeps the average
            // period exactly `delayMs`, at the cost of firing back-to-back
            // (across separate PopDue calls, never within one) to catch up
            // after a long pause, matching the guarantee documented on
            // Schedule().
            entries_[best].dueMs = fired.dueMs + fired.delayMs;
            entries_[best].firedInSweep = sweep;
        }
        else
        {
            entries_[best] = std::move(entries_[entries_.Size() - 1]);
            entries_.PopBack();
        }

        fired.callback(fired.userData);
        ++firedCount;
    }
}

} // namespace forge::core::platform
