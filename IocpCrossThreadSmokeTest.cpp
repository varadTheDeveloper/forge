// Real-machine smoke test for IocpLoop::PostCompletion's documented
// cross-thread wakeup pattern — the single biggest open technical risk
// flagged in FsAsync.md (Phase 7.5's frozen design doc, Design Goals):
// IocpSmokeTest.cpp only ever posts a completion and calls RunOnce()
// immediately afterward, on the SAME thread. That proves
// PostCompletion()+RunOnce() dispatch works at all, but says nothing
// about the actual pattern Phase 7.5 fundamentally depends on — one
// thread genuinely blocked inside RunOnce()'s GetQueuedCompletionStatus
// wait while a DIFFERENT thread (a ThreadPool worker, in the real
// feature) calls PostCompletion() to wake it.
//
// IOCP is documented by Microsoft to support exactly this, and
// IocpLoop::PostCompletion's own doc comment says as much ("Not safe to
// call from another thread without additional synchronization —
// cross-thread wakeup is what PostCompletion() is for"), but this
// project's own bar everywhere else has been a real machine, not just
// documentation (File, Mutex/ConditionVariable, Socket all needed their
// own smoke test despite being equally well-documented Win32 APIs) — so
// this gets one too, before Phase 7.5 is allowed to build anything on
// top of the assumption. This test is Milestone 1 of Phase 7.5's
// incremental implementation, gating everything after it.
//
// Separate console-app project/target, same as IocpSmokeTest.cpp and
// every other *SmokeTest.cpp in this codebase — not part of the
// production moz.build build, Windows-only, cannot be compiled or run
// outside a real machine.
//
// Build (run from this file's own directory, so the forge-core\
// prefixed paths below resolve correctly). Needs IocpLoop.cpp and
// TimerScheduler.cpp linked in alongside Thread.cpp -- IoLoop.h pulls in
// IocpLoop's declarations, but not its (or TimerScheduler's) definitions,
// so omitting either one leaves unresolved IocpLoop::*/TimerScheduler::*
// linker symbols (confirmed on a real build, see HISTORY.md):
//   cl /std:c++17 /EHs- /W4 IocpCrossThreadSmokeTest.cpp
//   forge-core\platform\IocpLoop.cpp forge-core\platform\TimerScheduler.cpp
//   forge-core\Thread.cpp forge-core\platform\Win32Error.cpp
//   forge-core\memory\DefaultAllocator.cpp
//   forge-core\memory\detail\AllocationBackend.cpp /link ws2_32.lib
//
// Expected output if the cross-thread pattern genuinely works:
//   IocpCrossThreadSmokeTest: loop initialized
//   IocpCrossThreadSmokeTest: worker thread posting completion after delay...
//   IocpCrossThreadSmokeTest: RunOnce() woke up (bytes=99, key=7, ok=1)
//   IocpCrossThreadSmokeTest: all checks passed
//
// If the cross-thread pattern does NOT work as documented, RunOnce()
// blocks until the 5-second safety-net timer below fires instead, and
// the test reports a clear, bounded-time FAILURE rather than hanging
// forever:
//   IocpCrossThreadSmokeTest: TIMED OUT waiting for cross-thread
//   PostCompletion -- see FsAsync.md's Design Goals.

#include "forge-core/platform/IoLoop.h"
#include "forge-core/Thread.h"

#include <windows.h>

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

struct CompletionState
{
    bool fired{ false };
    DWORD bytesTransferred{ 0 };
    ULONG_PTR completionKey{ 0 };
    bool succeeded{ false };
};

struct TimeoutState
{
    IoLoop* loop{ nullptr };
    bool timedOut{ false };
};

} // namespace

int main()
{
    IoLoop loop;

    Result<void> initResult = loop.Initialize();
    CHECK(!initResult.HasError());
    std::printf("IocpCrossThreadSmokeTest: loop initialized\n");

    CompletionState completionState;
    IoCompletion completion{};
    completion.Pointer = &completionState; // OVERLAPPED::Pointer, unused for
                                            // a manually-posted completion,
                                            // reused purely as convenient
                                            // storage -- same idiom
                                            // IocpSmokeTest.cpp already uses.
    completion.callback = [](IoCompletion* self, DWORD bytesTransferred, ULONG_PTR completionKey, bool succeeded, DWORD /*errorCode*/) noexcept
    {
        auto* state = static_cast<CompletionState*>(self->Pointer);
        state->fired = true;
        state->bytesTransferred = bytesTransferred;
        state->completionKey = completionKey;
        state->succeeded = succeeded;
    };

    // Safety-net timer: if the cross-thread PostCompletion below never
    // actually wakes RunOnce() up, this fires after 5 seconds and stops
    // the loop, turning what would otherwise be an indefinite hang into a
    // bounded-time, clearly-reported failure instead.
    TimeoutState timeoutState;
    timeoutState.loop = &loop;

    Result<TimerId> timerResult = loop.ScheduleTimer(
        5000, /*repeat=*/false,
        [](void* userData) noexcept
        {
            auto* state = static_cast<TimeoutState*>(userData);
            state->timedOut = true;
            state->loop->RequestStop();
        },
        &timeoutState);
    CHECK(timerResult.HasValue());

    // The worker thread: sleeps briefly (so the main thread below is
    // genuinely already parked inside RunOnce()'s
    // GetQueuedCompletionStatus wait, not racing to post before RunOnce()
    // is even called), then posts the completion from this -- a
    // completely different OS thread than the one that will call
    // RunOnce(). This is the exact pattern IocpSmokeTest.cpp never
    // covered -- see this file's header comment.
    //
    // Deliberately does NOT call CHECK (and so does not touch the shared,
    // non-atomic g_failures) from this thread -- doing so could race with
    // the main thread's own CHECK calls around RunOnce() returning, since
    // nothing synchronizes "worker calls PostCompletion" with "worker's
    // very next statement" the way IOCP's own completion handoff
    // synchronizes the PostCompletion call itself with RunOnce() waking
    // up. Instead, the outcome is recorded in a plain bool and only read
    // on the main thread after Join(), which is a real synchronization
    // point.
    bool workerPostFailed = false;

    std::printf("IocpCrossThreadSmokeTest: worker thread posting completion after delay...\n");

    Result<Thread> worker = Thread::Create(
        [&loop, &completion, &workerPostFailed]()
        {
            ::Sleep(200);
            Result<void> postResult = loop.PostCompletion(&completion, 99, 7);
            if (postResult.HasError())
            {
                workerPostFailed = true;
            }
        });
    CHECK(worker.HasValue());

    if (!worker.HasValue())
    {
        return 1; // nothing further to test without a real worker thread
    }

    // Blocks here until either the worker thread's PostCompletion() wakes
    // it (the pattern under test) or the 5-second safety-net timer fires.
    Result<void> runOnceResult = loop.RunOnce();
    CHECK(!runOnceResult.HasError());

    CHECK(!worker.Value().Join().HasError());
    CHECK(!workerPostFailed);

    if (timeoutState.timedOut)
    {
        std::printf(
            "IocpCrossThreadSmokeTest: TIMED OUT waiting for cross-thread "
            "PostCompletion -- see FsAsync.md's Design Goals.\n");
        ++g_failures;
    }
    else
    {
        CHECK(completionState.fired);
        CHECK(completionState.bytesTransferred == 99);
        CHECK(completionState.completionKey == 7);
        CHECK(completionState.succeeded);

        if (timerResult.HasValue())
        {
            loop.CancelTimer(timerResult.Value());
        }

        std::printf(
            "IocpCrossThreadSmokeTest: RunOnce() woke up (bytes=%lu, key=%zu, ok=%d)\n",
            static_cast<unsigned long>(completionState.bytesTransferred),
            static_cast<size_t>(completionState.completionKey),
            completionState.succeeded ? 1 : 0);
    }

    if (g_failures == 0)
    {
        std::printf("IocpCrossThreadSmokeTest: all checks passed\n");
        return 0;
    }

    std::printf("IocpCrossThreadSmokeTest: %d CHECK(S) FAILED\n", g_failures);
    return 1;
}
