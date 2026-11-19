#pragma once

#include "../Types.h"
#include "../Result.h"
#include "../memory/Vector.h"

namespace forge::core::platform
{

using TimerId = u64;
using TimerCallback = void (*)(void* userData);

/// Pure timer bookkeeping: no OS calls, no threading, no I/O.
///
/// An IoLoop backend (IocpLoop today, an epoll/io_uring backend later)
/// wraps this to decide how long to block waiting for the next OS event,
/// and calls PopDue() after waking to find out what fired. Keeping this
/// logic platform-independent means it can be fully unit tested without
/// any OS-specific dependency, and reused unchanged by every future
/// backend.
///
/// Time is an opaque "milliseconds since some fixed point" u64 — the
/// caller picks the clock (steady_clock in practice) and must pass
/// consistent values into Schedule()/PopDue()/NextDueDelay().
class TimerScheduler
{
public:

    TimerScheduler() = default;

    TimerScheduler(const TimerScheduler&) = delete;
    TimerScheduler& operator=(const TimerScheduler&) = delete;

    /// Registers a timer due at `nowMs + delayMs`. If `repeat` is true it
    /// keeps firing every `delayMs` (measured from when it was due, not
    /// from when PopDue() happened to run) until Cancel()'d.
    [[nodiscard]]
    Result<TimerId> Schedule(
        u64 nowMs,
        u64 delayMs,
        bool repeat,
        TimerCallback callback,
        void* userData) noexcept;

    /// No-op if `id` doesn't exist or already fired-and-was-one-shot.
    /// Safe to call from inside a callback currently running via PopDue()
    /// for a *different* timer id, or for its own id (a repeating timer
    /// cancelling itself during its own callback will not be
    /// rescheduled).
    void Cancel(TimerId id) noexcept;

    [[nodiscard]]
    bool Empty() const noexcept;

    [[nodiscard]]
    Size Count() const noexcept;

    /// Writes the number of milliseconds until the next timer is due
    /// (0 if one is already overdue) into `outDelayMs` and returns true.
    /// Returns false (leaving `outDelayMs` untouched) if there are no
    /// timers at all — the backend should then wait indefinitely for the
    /// next OS event instead of polling.
    [[nodiscard]]
    bool NextDueDelay(u64 nowMs, u64& outDelayMs) const noexcept;

    /// Invokes every timer due at or before `nowMs`, removing one-shot
    /// timers and rescheduling repeating ones. Timers fire in order of due
    /// time; timers due at the same time fire in the order they were
    /// scheduled. A repeating timer fires at most once per call. A timer that cancels a
    /// *different, not-yet-due* timer from within its own callback is
    /// handled correctly; a timer that cancels itself is simply not
    /// rescheduled. Returns the number of callbacks invoked.
    Size PopDue(u64 nowMs) noexcept;

private:

    struct Entry
    {
        TimerId id{0};
        u64 dueMs{0};
        u64 delayMs{0};
        bool repeat{false};
        bool cancelled{false};
        // The PopDue() call (sweep_) in which a repeating timer last fired.
        u64 firedInSweep{0};
        TimerCallback callback{nullptr};
        void* userData{nullptr};
    };

    [[nodiscard]]
    Size IndexOf(TimerId id) const noexcept;

    Vector<Entry> entries_;
    TimerId nextId_{1};
    u64 sweep_{0};
};

} // namespace forge::core::platform
