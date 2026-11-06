// Real-thread smoke test for Mutex/ConditionVariable/Thread/ThreadPool —
// same role as IocpSmokeTest.cpp/FileSmokeTest.cpp: this spawns actual OS
// threads and exercises real mutual exclusion, so it can ONLY be built
// and run on an actual Windows machine (e.g. via a standalone Visual
// Studio project, same as IocpSmokeTest.cpp/FileSmokeTest.cpp), never in
// the Linux sandbox this was written in. Not part of the production
// moz.build build.
//
// Build (run from this file's own directory, forge-core\, so the
// unprefixed paths below resolve correctly):
//   cl /std:c++17 /EHs- /W4 ThreadingSmokeTest.cpp
//   Mutex.cpp ConditionVariable.cpp Thread.cpp ThreadPool.cpp
//   platform\Win32Error.cpp memory\DefaultAllocator.cpp
//   memory\detail\AllocationBackend.cpp

#include "ConditionVariable.h"
#include "LockGuard.h"
#include "Mutex.h"
#include "Thread.h"
#include "ThreadPool.h"

#include <cstdio>
#include <cstdlib>
#include <utility>

#include "memory/DefaultAllocator.h"

using namespace forge::core;
using namespace forge::core::memory;

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

/// Spawns `threadCount` threads, each incrementing `*counter` `iterations`
/// times under `mutex`. Without correct mutual exclusion this reliably
/// loses increments under real OS scheduling — a Mutex bug (or a bug in
/// how a caller uses it) shows up here as `*counter != threadCount *
/// iterations`, not as a crash.
void RunMutexStressTest()
{
    std::printf("RunMutexStressTest...\n");

    constexpr int kThreadCount = 8;
    constexpr int kIterations = 20000;

    Mutex mutex;
    long counter = 0;

    Thread threads[kThreadCount];

    for (int i = 0; i < kThreadCount; ++i)
    {
        Result<Thread> spawned = Thread::Create([&mutex, &counter, kIterations]() {
            for (int j = 0; j < kIterations; ++j)
            {
                LockGuard<Mutex> guard(mutex);
                ++counter;
            }
        });

        CHECK(spawned.HasValue());

        if (spawned.HasValue())
        {
            threads[i] = std::move(spawned.Value());
        }
    }

    for (int i = 0; i < kThreadCount; ++i)
    {
        if (threads[i].Joinable())
        {
            CHECK(!threads[i].Join().HasError());
        }
    }

    CHECK(counter == static_cast<long>(kThreadCount) * kIterations);
}

/// Classic single-producer/single-consumer handoff over a ConditionVariable
/// — exercises Wait()/NotifyOne() actually blocking and waking correctly,
/// not just typechecking against the Win32 API shape.
void RunConditionVariableProducerConsumerTest()
{
    std::printf("RunConditionVariableProducerConsumerTest...\n");

    Mutex mutex;
    ConditionVariable dataReady;
    bool ready = false;
    int payload = 0;

    Result<Thread> consumer = Thread::Create([&]() {
        LockGuard<Mutex> guard(mutex);
        while (!ready)
        {
            dataReady.Wait(mutex).Ignore();
        }
    });

    CHECK(consumer.HasValue());

    {
        LockGuard<Mutex> guard(mutex);
        payload = 42;
        ready = true;
    }

    dataReady.NotifyOne();

    if (consumer.HasValue())
    {
        CHECK(!consumer.Value().Join().HasError());
    }

    CHECK(payload == 42);
    CHECK(ready);
}

/// WaitFor should time out (returning false) when nobody ever notifies.
void RunConditionVariableTimeoutTest()
{
    std::printf("RunConditionVariableTimeoutTest...\n");

    Mutex mutex;
    ConditionVariable neverNotified;

    LockGuard<Mutex> guard(mutex);
    Result<bool> waited = neverNotified.WaitFor(mutex, /*timeoutMs=*/50);

    CHECK(waited.HasValue());
    CHECK(waited.Value() == false);
}

/// Submits many tasks across a small pool and verifies every one ran
/// exactly once — the real end-to-end signal ThreadPool needs, since
/// nothing in sandbox verification could exercise multiple worker
/// threads actually racing to pull from the shared queue.
void RunThreadPoolTest()
{
    std::printf("RunThreadPoolTest...\n");

    constexpr int kTaskCount = 500;

    DefaultAllocator allocator;
    ThreadPool pool;

    Result<void> initialized = pool.Initialize(allocator, /*threadCount=*/4);
    CHECK(!initialized.HasError());
    CHECK(pool.ThreadCount() == 4);

    Mutex mutex;
    int completed = 0;

    for (int i = 0; i < kTaskCount; ++i)
    {
        Result<void> submitted = pool.Submit([&mutex, &completed]() {
            LockGuard<Mutex> guard(mutex);
            ++completed;
        });

        CHECK(!submitted.HasError());
    }

    pool.Shutdown(); // drains the queue, then joins every worker

    CHECK(completed == kTaskCount);

    // Submitting after Shutdown() must fail, not silently succeed or crash.
    Result<void> submittedAfterShutdown = pool.Submit([]() {});
    CHECK(submittedAfterShutdown.HasError());
}

} // namespace

int main()
{
    RunMutexStressTest();
    RunConditionVariableProducerConsumerTest();
    RunConditionVariableTimeoutTest();
    RunThreadPoolTest();

    if (g_failures == 0)
    {
        std::printf("ThreadingSmokeTest: all checks passed\n");
        return 0;
    }

    std::printf("ThreadingSmokeTest: %d CHECK(S) FAILED\n", g_failures);
    return 1;
}
