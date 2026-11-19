// Standalone correctness test for TimerScheduler — pure logic, no OS calls,
// so (unlike the IOCP glue that will wrap this) it can be fully verified
// here with g++/clang++ under ASan/UBSan before ever touching Windows.
//
// Not part of the moz.build/mach production build — this is a throwaway
// verification harness, compiled and run directly, the same way
// VectorSmokeTest.cpp was used to verify Vector/Result/MakeUnique earlier.

#include "TimerScheduler.h"

#include <cstdio>
#include <cstdlib>

using namespace forge::core;
using namespace forge::core::platform;

namespace
{

int g_failures = 0;

#define CHECK(cond)                                                          \
    do                                                                       \
    {                                                                        \
        if (!(cond))                                                        \
        {                                                                    \
            std::fprintf(stderr, "CHECK FAILED at %s:%d: %s\n", __FILE__,    \
                          __LINE__, #cond);                                  \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

//==============================================================================
// Test 1: one-shot timer fires exactly once and is removed.
//==============================================================================

void Test_OneShotFiresOnceAndIsRemoved()
{
    std::printf("Test_OneShotFiresOnceAndIsRemoved...\n");

    TimerScheduler sched;
    int fireCount = 0;

    auto callback = [](void* userData) noexcept
    {
        ++(*static_cast<int*>(userData));
    };

    Result<TimerId> id = sched.Schedule(0, 100, /*repeat=*/false, callback, &fireCount);
    CHECK(id.HasValue());
    CHECK(sched.Count() == 1);
    CHECK(!sched.Empty());

    // Not due yet at t=50.
    Size fired = sched.PopDue(50);
    CHECK(fired == 0);
    CHECK(fireCount == 0);
    CHECK(sched.Count() == 1);

    // Due at t=100.
    fired = sched.PopDue(100);
    CHECK(fired == 1);
    CHECK(fireCount == 1);
    CHECK(sched.Count() == 0);
    CHECK(sched.Empty());

    // Popping again does nothing — it was removed, not just marked fired.
    fired = sched.PopDue(1000);
    CHECK(fired == 0);
    CHECK(fireCount == 1);
}

//==============================================================================
// Test 2: repeating timer fires multiple times with correctly advancing
// due times (due-time based, not "now + delay" based, so it doesn't drift
// under repeated late polling).
//==============================================================================

void Test_RepeatingTimerAdvancesDueTime()
{
    std::printf("Test_RepeatingTimerAdvancesDueTime...\n");

    TimerScheduler sched;
    int fireCount = 0;

    auto callback = [](void* userData) noexcept
    {
        ++(*static_cast<int*>(userData));
    };

    Result<TimerId> id = sched.Schedule(0, 10, /*repeat=*/true, callback, &fireCount);
    CHECK(id.HasValue());

    // First due at t=10.
    CHECK(sched.PopDue(10) == 1);
    CHECK(fireCount == 1);
    CHECK(sched.Count() == 1); // still scheduled: it repeats

    // Not due again until t=20.
    CHECK(sched.PopDue(15) == 0);
    CHECK(fireCount == 1);

    // Due time is anchored to when it was *due* (10 + 10 = 20), not to when
    // PopDue happened to be called — firing late at t=23 should not shift
    // the next due time to 33, it should still be 30.
    CHECK(sched.PopDue(23) == 1);
    CHECK(fireCount == 2);

    u64 outDelay = 0;
    CHECK(sched.NextDueDelay(23, outDelay));
    CHECK(outDelay == 7); // next due at 30, now is 23 -> 7ms away

    CHECK(sched.PopDue(30) == 1);
    CHECK(fireCount == 3);
    CHECK(sched.Count() == 1);
}

//==============================================================================
// Test 3: cancelling a timer before it's due removes it without firing.
//==============================================================================

void Test_CancelBeforeDue()
{
    std::printf("Test_CancelBeforeDue...\n");

    TimerScheduler sched;
    int fireCount = 0;

    auto callback = [](void* userData) noexcept
    {
        ++(*static_cast<int*>(userData));
    };

    Result<TimerId> id = sched.Schedule(0, 100, /*repeat=*/false, callback, &fireCount);
    CHECK(id.HasValue());

    sched.Cancel(id.Value());
    CHECK(sched.Count() == 0); // cancelled entries don't count
    CHECK(sched.Empty());

    Size fired = sched.PopDue(1000);
    CHECK(fired == 0);
    CHECK(fireCount == 0);

    // Cancelling an unknown/already-gone id is a safe no-op — must not
    // crash or corrupt state; ASan/UBSan will catch it if it does.
    sched.Cancel(TimerId(999999));
    sched.Cancel(id.Value()); // already gone after PopDue swept it
    CHECK(sched.Empty());
}

//==============================================================================
// Test 4: a callback cancels a *different* timer from within PopDue — both
// a not-yet-processed target and an already-fired-and-removed one.
//==============================================================================

struct CancelOtherState
{
    TimerScheduler* sched;
    TimerId otherIdNotYetDue;
    TimerId otherIdAlreadyFired;
    int otherFireCount;
    int selfFireCount;
};

void Test_CancelOtherTimerFromCallback()
{
    std::printf("Test_CancelOtherTimerFromCallback...\n");

    TimerScheduler sched;
    CancelOtherState state{};
    state.sched = &sched;
    state.otherFireCount = 0;
    state.selfFireCount = 0;

    auto otherCallback = [](void* userData) noexcept
    {
        ++static_cast<CancelOtherState*>(userData)->otherFireCount;
    };

    // "already fired" timer: due earlier, fires and is removed before the
    // canceller runs (both due at the same PopDue(100) call, but the
    // canceller is scheduled second so it processes after — see ordering
    // note below; either way Cancel() on an id that's already gone must be
    // a safe no-op).
    Result<TimerId> alreadyFired = sched.Schedule(0, 50, false, otherCallback, &state);
    CHECK(alreadyFired.HasValue());
    state.otherIdAlreadyFired = alreadyFired.Value();

    // "not yet due" timer: due much later, still pending when the
    // canceller runs.
    Result<TimerId> notYetDue = sched.Schedule(0, 5000, false, otherCallback, &state);
    CHECK(notYetDue.HasValue());
    state.otherIdNotYetDue = notYetDue.Value();

    // Fire and remove the "already fired" one first, independently, so it's
    // genuinely gone before the canceller timer runs.
    CHECK(sched.PopDue(50) == 1);
    CHECK(state.otherFireCount == 1);
    CHECK(sched.Count() == 1); // only notYetDue remains

    auto cancellerCallback = [](void* userData) noexcept
    {
        auto* s = static_cast<CancelOtherState*>(userData);
        ++s->selfFireCount;
        s->sched->Cancel(s->otherIdNotYetDue);     // still pending -> cancel it
        s->sched->Cancel(s->otherIdAlreadyFired);   // already gone -> no-op, must not crash
    };

    Result<TimerId> canceller = sched.Schedule(60, 10, false, cancellerCallback, &state);
    CHECK(canceller.HasValue());

    CHECK(sched.PopDue(70) == 1); // only the canceller is due; notYetDue is due at 5060
    CHECK(state.selfFireCount == 1);
    CHECK(sched.Count() == 0); // notYetDue got cancelled inside the callback

    // Confirm notYetDue really never fires, even once its original due time
    // would have passed.
    CHECK(sched.PopDue(6000) == 0);
    CHECK(state.otherFireCount == 1); // unchanged
}

//==============================================================================
// Test 5: a repeating timer cancels *itself* from within its own callback —
// it must not be rescheduled or fire again.
//==============================================================================

struct SelfCancelState
{
    TimerScheduler* sched;
    TimerId selfId;
    int fireCount;
};

void Test_SelfCancelStopsRepeating()
{
    std::printf("Test_SelfCancelStopsRepeating...\n");

    TimerScheduler sched;
    SelfCancelState state{};
    state.sched = &sched;
    state.fireCount = 0;

    auto callback = [](void* userData) noexcept
    {
        auto* s = static_cast<SelfCancelState*>(userData);
        ++s->fireCount;
        if (s->fireCount == 2)
        {
            s->sched->Cancel(s->selfId); // cancel self on the 2nd firing
        }
    };

    Result<TimerId> id = sched.Schedule(0, 10, /*repeat=*/true, callback, &state);
    CHECK(id.HasValue());
    state.selfId = id.Value();

    CHECK(sched.PopDue(10) == 1);
    CHECK(state.fireCount == 1);
    CHECK(sched.Count() == 1); // still repeating

    CHECK(sched.PopDue(20) == 1);
    CHECK(state.fireCount == 2);
    CHECK(sched.Count() == 0); // cancelled itself on this firing

    // Confirm it truly does not fire a 3rd time even far in the future.
    CHECK(sched.PopDue(1000000) == 0);
    CHECK(state.fireCount == 2);
}

//==============================================================================
// Test 6: NextDueDelay reports the minimum across concurrent timers, and
// false when none are pending (including when all remaining are cancelled).
//==============================================================================

void Test_NextDueDelayAcrossConcurrentTimers()
{
    std::printf("Test_NextDueDelayAcrossConcurrentTimers...\n");

    TimerScheduler sched;

    u64 outDelay = 999;
    CHECK(!sched.NextDueDelay(0, outDelay)); // nothing scheduled yet

    auto noop = [](void*) noexcept {};

    Result<TimerId> a = sched.Schedule(0, 500, false, noop, nullptr);
    Result<TimerId> b = sched.Schedule(0, 100, false, noop, nullptr); // earliest
    Result<TimerId> c = sched.Schedule(0, 300, false, noop, nullptr);
    CHECK(a.HasValue() && b.HasValue() && c.HasValue());

    CHECK(sched.NextDueDelay(0, outDelay));
    CHECK(outDelay == 100);

    // Already overdue relative to "now" -> delay clamps to 0, not underflow.
    CHECK(sched.NextDueDelay(150, outDelay));
    CHECK(outDelay == 0);

    // Cancel the earliest; next-earliest (c, due at 300) should now win.
    sched.Cancel(b.Value());
    CHECK(sched.NextDueDelay(0, outDelay));
    CHECK(outDelay == 300);

    // Cancel everything remaining -> no timers pending again.
    sched.Cancel(a.Value());
    sched.Cancel(c.Value());
    CHECK(!sched.NextDueDelay(0, outDelay));
}

//==============================================================================
// Test 7: scheduling a *new* timer from within a callback during PopDue is
// safe even if it forces the backing Vector to reallocate mid-iteration.
//==============================================================================

struct GrowMidIterationState
{
    TimerScheduler* sched;
    int spawnedFireCount;
    int spawnerFireCount;
    int spawnRemaining;
};

void SpawnedCallback(void* userData) noexcept
{
    ++static_cast<GrowMidIterationState*>(userData)->spawnedFireCount;
}

void SpawnerCallback(void* userData) noexcept
{
    auto* s = static_cast<GrowMidIterationState*>(userData);
    ++s->spawnerFireCount;

    // Schedule a batch of new, already-due timers from inside the
    // callback. This forces entries_ to grow (repeatedly, past its
    // current capacity) while PopDue's own iteration index is live — the
    // scenario the forward "re-query Size() each loop check" idiom exists
    // for. If PopDue cached entries_.Size() once at loop entry, these
    // freshly-spawned due timers would silently never be visited this
    // sweep; if the vector reallocated during Schedule() and PopDue held a
    // dangling reference/pointer into the old buffer, ASan would catch it.
    for (int i = 0; i < 64; ++i)
    {
        Result<TimerId> r = s->sched->Schedule(0, 0, false, &SpawnedCallback, s);
        (void)r;
    }
}

void Test_ScheduleFromCallbackDuringPopDue()
{
    std::printf("Test_ScheduleFromCallbackDuringPopDue...\n");

    TimerScheduler sched;
    GrowMidIterationState state{};
    state.sched = &sched;
    state.spawnedFireCount = 0;
    state.spawnerFireCount = 0;
    state.spawnRemaining = 0;

    Result<TimerId> spawner = sched.Schedule(0, 0, /*repeat=*/false, &SpawnerCallback, &state);
    CHECK(spawner.HasValue());

    // A single PopDue sweep: the spawner fires first (it was scheduled
    // first, so IndexOf/PopDue visits it first), spawning 64 new due
    // timers mid-sweep. All 64 must still be visited and fired within this
    // same PopDue call, since they're already due at nowMs=0.
    Size fired = sched.PopDue(0);

    CHECK(state.spawnerFireCount == 1);
    CHECK(state.spawnedFireCount == 64);
    CHECK(fired == 65); // spawner + all 64 spawned
    CHECK(sched.Empty());
    CHECK(sched.Count() == 0);
}

//==============================================================================
// Test 8 (Phase 10.3): due timers fire in order of due time, and timers due
// at the same time in the order they were scheduled. Swap-removal used to
// scramble this: three timers due together, behind one not yet due, fired
// in reverse (found by tests/web-abort.js, "AbortSignal timeouts fire in
// order").
//==============================================================================

struct OrderState
{
    char order[16];
    int count;
};

template <char Tag>
void RecordOrder(void* userData) noexcept
{
    auto* s = static_cast<OrderState*>(userData);
    if (s->count < 15)
    {
        s->order[s->count++] = Tag;
        s->order[s->count] = '\0';
    }
}

bool SameText(const char* a, const char* b)
{
    while (*a != '\0' && *a == *b)
    {
        ++a;
        ++b;
    }
    return *a == *b;
}

void Test_DueTimersFireInOrder()
{
    std::printf("Test_DueTimersFireInOrder...\n");

    {
        TimerScheduler sched;
        OrderState state{};
        CHECK(sched.Schedule(0, 1000, false, &RecordOrder<'x'>, &state).HasValue());
        CHECK(sched.Schedule(0, 5, false, &RecordOrder<'1'>, &state).HasValue());
        CHECK(sched.Schedule(0, 5, false, &RecordOrder<'2'>, &state).HasValue());
        CHECK(sched.Schedule(0, 5, false, &RecordOrder<'3'>, &state).HasValue());
        CHECK(sched.PopDue(10) == 3);
        CHECK(SameText(state.order, "123"));  // vector order gave "132"
        CHECK(sched.Count() == 1);
    }

    {
        // Earlier due time first, whatever the scheduling order.
        TimerScheduler sched;
        OrderState state{};
        CHECK(sched.Schedule(0, 30, false, &RecordOrder<'c'>, &state).HasValue());
        CHECK(sched.Schedule(0, 10, false, &RecordOrder<'a'>, &state).HasValue());
        CHECK(sched.Schedule(0, 20, false, &RecordOrder<'b'>, &state).HasValue());
        CHECK(sched.Schedule(0, 10, true, &RecordOrder<'r'>, &state).HasValue());
        // r (repeating, due at 10) fires once here, after a (same due time,
        // scheduled earlier), even though it is due again at 20 and 30.
        CHECK(sched.PopDue(50) == 4);
        CHECK(SameText(state.order, "arbc"));
        CHECK(sched.Count() == 1);
    }
}

} // namespace

int main()
{
    Test_OneShotFiresOnceAndIsRemoved();
    Test_RepeatingTimerAdvancesDueTime();
    Test_CancelBeforeDue();
    Test_CancelOtherTimerFromCallback();
    Test_SelfCancelStopsRepeating();
    Test_NextDueDelayAcrossConcurrentTimers();
    Test_ScheduleFromCallbackDuringPopDue();
    Test_DueTimersFireInOrder();

    if (g_failures == 0)
    {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }

    std::printf("%d CHECK(S) FAILED\n", g_failures);
    return 1;
}
