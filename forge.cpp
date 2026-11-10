#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "js/CallAndConstruct.h"
#include "js/String.h"
#include <string>
#include <utility>
#include "js/CompileOptions.h"
#include "js/CompilationAndEvaluation.h"
#include "js/Context.h"
#include "js/GCAPI.h"
#include "js/GlobalObject.h"
#include "js/Initialization.h"
#include "js/RealmOptions.h"
#include "js/RootingAPI.h"
#include "js/SourceText.h"
#include "js/Promise.h"
#include "js/Object.h"
#include "js/TracingAPI.h"
#include "js/Value.h"

#include "js/CallArgs.h"
#include "js/CharacterEncoding.h"
#include "js/PropertyAndElement.h"
#include "js/Conversions.h"
#include "js/Exception.h"
#include "js/ErrorReport.h"
#include "js/ArrayBuffer.h"
#include "js/experimental/TypedData.h"
#include "js/PropertyDescriptor.h"  // JSPROP_ENUMERATE (Phase 7.3, fs.statSync)

// Phase 7.9 (all confirmed by reading the headers in this tree):
//   js/ContextOptions.h      -- ContextOptions::setAsyncStack*()
//   js/GCVector.h            -- JS::StackGCVector, named by JSCSPEvalChecker
//   js/Principals.h          -- JSSecurityCallbacks / JSCSPEvalChecker
//   js/Stack.h               -- JS::BuildStackString (stack traces)
//   js/friend/ErrorMessages.h -- js::GetErrorMessage / JSMSG_NOT_FUNCTION
#include "js/ContextOptions.h"
#include "js/GCVector.h"
#include "js/Principals.h"
#include "js/Stack.h"
#include "js/friend/ErrorMessages.h"

// Phase 7.95: SpiderMonkey ESR 153 owns the microtask queue and exposes it
// through this header (RunJSMicroTask, EnqueueMicroTask, ...). The older
// 142 Nightly engine, where the embedder owned the queue, is no longer
// supported; see HISTORY.md, "Phase 7.95".
#include "js/friend/MicroTask.h"

// JS_NewPlainObject (Phase 7.3, fs's namespace object and statSync's
// returned {size} object) is not exposed under js/public -- confirmed by
// reading js/public/Object.h directly, which has no object-construction
// free function. It lives in jsapi.h instead (js/src/jsapi.h, confirmed by
// reading it directly), the classic top-level SpiderMonkey embedding
// header every other js/public/*.h include in this file was deliberately
// kept narrower than -- included here just for this one function.
#include "jsapi.h"

#include "forge-core/platform/IoLoop.h"
#include "forge-core/platform/Process.h"
#include "forge-core/Assert.h"
#include "forge-core/File.h"
#include "forge-core/HashMap.h"
#include "forge-core/Path.h"
#include "forge-core/Result.h"
#include "forge-core/String.h"
#include "forge-core/StringView.h"
#include "forge-core/ThreadPool.h"
#include "forge-core/memory/DefaultAllocator.h"
#include "forge-core/memory/MakeUnique.h"
#include "forge-core/memory/UniquePtr.h"
#include "forge-core/memory/Vector.h"

// cx points to SpiderMonkey's context object.
static const JSClass globalClass = {
    "global",
    JSCLASS_GLOBAL_FLAGS | JSCLASS_HAS_RESERVED_SLOTS(1),
    &JS::DefaultGlobalClassOps,
};

enum GlobalSlots { RuntimeSlot = JSCLASS_GLOBAL_SLOT_COUNT };
static bool EnqueueMicrotask(JSContext* cx, JS::HandleObject callback);

// Forward declarations: ForgeTimerFired (defined further down, right after
// JsTimerRegistry) needs to reach the current Runtime's timer registry,
// but Runtime itself is only fully defined later in this file (it embeds
// JsTimerRegistry by value, so it can't be forward-declared-only at that
// point). Same pattern already used for EnqueueMicrotask above.
class JsTimerRegistry;
static JsTimerRegistry& GetRuntimeTimers(JSContext* cx);

//==============================================================================
// Process state (Phase 7.9)
//
// Forge follows Node's rules for fatal errors: an uncaught exception (in
// the main script, a timer callback or a microtask) or an unhandled
// Promise rejection prints the error with its stack trace and ends the
// process with exit code 1. Before Phase 7.9 a throwing microtask was never
// removed from the queue, unhandled rejections were silently ignored, and
// the process exited 0 in both cases.
//
// g_stopRequested makes the event loop stop as soon as a fatal error has
// been reported; nothing else runs after it.
//==============================================================================

static int g_exitCode = 0;
static bool g_stopRequested = false;

// Runtime code-generation policy, consulted by the engine's security
// callback (ForgeCodeGenerationAllowed below) for every eval(), new
// Function(...) and WebAssembly compilation. Both default to allowed;
// the CLI flags --disallow-code-generation-from-strings and
// --disallow-wasm turn them off. Kavach (Phase 9) will make this per
// package; for now the hook exists and is enforced process-wide.
struct CodeGenerationPolicy {
  bool allowStrings{true};
  bool allowWasm{true};
};

static CodeGenerationPolicy g_codeGenPolicy;

static bool ForgeCodeGenerationAllowed(
    JSContext* cx, JS::RuntimeCode kind, JS::Handle<JSString*> codeString,
    JS::CompilationType compilationType,
    JS::Handle<JS::StackGCVector<JSString*>> parameterStrings,
    JS::Handle<JSString*> bodyString,
    JS::Handle<JS::StackGCVector<JS::Value>> parameterArgs,
    JS::Handle<JS::Value> bodyArg, bool* outCanCompileStrings) {
  (void)cx;
  (void)codeString;
  (void)compilationType;
  (void)parameterStrings;
  (void)bodyString;
  (void)parameterArgs;
  (void)bodyArg;

  *outCanCompileStrings =
      kind == JS::RuntimeCode::WASM ? g_codeGenPolicy.allowWasm : g_codeGenPolicy.allowStrings;
  return true;
}

static const JSSecurityCallbacks kForgeSecurityCallbacks = {
    ForgeCodeGenerationAllowed,  // contentSecurityPolicyAllows
    nullptr,                     // codeForEvalGets (spec default)
    nullptr,                     // subsumes (single principal for now)
};

// Prints a saved-frame stack object (the stack captured when an exception
// was thrown) to stderr. Silent if there is no stack.
static void PrintExceptionStack(JSContext* cx, JS::HandleObject stack) {
  if (!stack) {
    return;
  }

  JS::RootedString str(cx);

  if (!JS::BuildStackString(cx, nullptr, stack, &str, 2) || !str) {
    JS_ClearPendingException(cx);
    return;
  }

  JS::UniqueChars utf8 = JS_EncodeStringToUTF8(cx, str);

  if (!utf8) {
    JS_ClearPendingException(cx);
    return;
  }

  fprintf(stderr, "Stack:\n%s", utf8.get());
}

// Prints an exception value plus its stack (Phase 7.9).
static void PrintExceptionReport(JSContext* cx, const JS::ExceptionStack& exn,
                                 const char* context) {
  JS::ErrorReportBuilder report(cx);

  if (!report.init(cx, exn, JS::ErrorReportBuilder::NoSideEffects)) {
    JS_ClearPendingException(cx);
    fprintf(stderr, "Forge: %s failed (could not describe the error)\n", context);
    return;
  }

  JS::PrintError(stderr, report, false);
  PrintExceptionStack(cx, exn.stack());
}

// Prints (and consumes) whatever exception is currently pending on `cx`,
// including its stack trace (Phase 7.9), tagged with `context` for the
// fallback cases. Shared by every callback boundary that calls into JS and
// cannot propagate a C++ exception on failure (there are no C++ exceptions
// in this codebase — see forge-core's Result<T>/AGENTS.md philosophy; this
// is the JS-engine-boundary equivalent: report, don't throw, don't
// silently drop it).
static void ReportPendingException(JSContext* cx, const char* context) {
  if (!JS_IsExceptionPending(cx)) {
    // JS::Call/Evaluate returned false with nothing pending: an
    // uncatchable error (for example, the engine terminated the script).
    fprintf(stderr, "Forge: %s terminated by an uncatchable error\n", context);
    return;
  }

  JS::ExceptionStack exn(cx);

  if (!JS::StealPendingExceptionStack(cx, &exn)) {
    JS_ClearPendingException(cx);
    fprintf(stderr, "Forge: %s failed (exception pending, but could not be retrieved)\n",
            context);
    return;
  }

  PrintExceptionReport(cx, exn, context);
}

// Reports the pending exception as uncaught and makes the process exit
// with code 1 once the event loop unwinds.
static void ReportFatalException(JSContext* cx, const char* context) {
  fprintf(stderr, "Uncaught exception in %s:\n", context);
  ReportPendingException(cx, context);
  g_exitCode = 1;
  g_stopRequested = true;
}

// Throws a TypeError "<what> is not a function" on cx.
static bool ThrowNotFunction(JSContext* cx, const char* what) {
  JS_ReportErrorNumberASCII(cx, js::GetErrorMessage, nullptr, JSMSG_NOT_FUNCTION, what);
  return false;
}

//==============================================================================
// Runtime Integration (Phase 6)
//
// The timer registry below is backed by forge-core's own containers
// (HashMap<K,V>) rather than std::unordered_map, and SetTimeout/SetInterval
// go through forge-core's MakeUnique<T> (Result<UniquePtr<T>>) rather than
// std::make_unique, so that an allocation failure is a reported JS OOM
// error instead of an abrupt std::terminate() (this codebase's
// -fno-exceptions build has no other way to recover from operator new
// failing). See HISTORY.md's Phase 6 entry for the bug this fixes:
// JsTimerRegistry::Add() schedules the native timer before it knows
// whether the JS-side wrapper can actually be stored. If storing it fails,
// the *already-armed* native timer must be cancelled before returning, or
// it fires against memory that's about to be freed. See the comment in
// Add() below.
//
// Until Phase 7.95 Forge also kept its own microtask queue here
// (Queue<UniquePtr<Microtask>>, traced from TraceForgeRoots). The engine
// owns that queue now; see ForgeJobQueue below.
//==============================================================================

// Job queue (Phase 7.95). The engine owns the microtask queue
// (js/friend/MicroTask.h); Forge decides when and how it is drained.
// Promise jobs are engine "JS microtasks" that must be run with
// JS::RunJSMicroTask inside the realm of JS::GetExecutionGlobalFromJSMicroTask.
// queueMicrotask() callbacks are enqueued by Forge as plain function values;
// the engine traces every queued value, so Forge keeps no queue or GC roots
// of its own. The drain policy is unchanged from Phase 7.9: jobs are popped
// before they run, JobQueueIsEmpty before the last one, ClearKeptObjects
// after the drain, and an exception is fatal (exit code 1).
// Mirrors InternalJobQueue::runJobs in ESR 153's js/src/vm/JSContext.cpp.

class ForgeJobQueue : public JS::JobQueue {
 public:
  bool getHostDefinedData(JSContext* cx, JS::MutableHandle<JSObject*> incumbentGlobal,
                          JS::MutableHandle<JSObject*> optionalHostDefinedData) const override {
    (void)cx;
    incumbentGlobal.set(nullptr);
    optionalHostDefinedData.set(nullptr);
    return true;
  }

  bool getHostDefinedGlobal(JSContext* cx, JS::MutableHandle<JSObject*> data) const override {
    (void)cx;
    data.set(nullptr);
    return true;
  }

  bool isDrainingStopped() const override { return false; }

  void runJobs(JSContext* cx) override {
    JS::Rooted<JS::GenericMicroTask> next(cx);

    while (!g_stopRequested && JS::HasAnyMicroTasks(cx)) {
      next = JS::DequeueNextMicroTask(cx);

      if (!JS::HasAnyMicroTasks(cx)) {
        JS::JobQueueIsEmpty(cx);
      }

      bool ok = true;

      if (JS::IsJSMicroTask(next)) {
        JS::Rooted<JS::JSMicroTask*> job(cx, JS::ToMaybeWrappedJSMicroTask(next));
        JS::RootedObject global(cx, JS::GetExecutionGlobalFromJSMicroTask(job));

        if (!global) {
          continue;  // dead wrapper: the engine skips these too
        }

        JSAutoRealm ar(cx, global);
        ok = JS::RunJSMicroTask(cx, job);
      } else if (next.isObject()) {
        // A queueMicrotask() callback.
        JS::RootedObject callback(cx, &next.toObject());
        JS::RootedValue rval(cx);
        JSAutoRealm ar(cx, callback);
        ok = JS::Call(cx, JS::UndefinedHandleValue, callback, JS::HandleValueArray::empty(),
                      &rval);
      }

      if (!ok) {
        ReportFatalException(cx, "microtask");
      }
    }

    JS::JobQueueMayNotBeEmpty(cx);
    JS::ClearKeptObjects(cx);
  }

 protected:
  js::UniquePtr<SavedJobQueue> saveJobQueue(JSContext* cx) override {
    (void)cx;
    return nullptr;  // only the Debugger API uses this; Forge has none yet
  }
};


//==============================================================================
// Timers
//
// Bridges the JS-facing setTimeout/setInterval/clearTimeout API onto
// forge::core::platform::IoLoop's TimerId-based scheduling (see
// ROADMAP.md Phase 2 / HISTORY.md — this replaces the old TimerQueue's
// manual "scan every timer, compare to now()" polling, driven by
// EventLoop's busy-wait, with the IOCP-backed loop actually sleeping until
// the next timer or I/O event).
//
// Two id spaces are kept deliberately separate: `jsId` is the small
// integer setTimeout()/setInterval() return to script (unchanged
// behaviour); `nativeId` is IoLoop's own forge::core::platform::TimerId,
// used only internally to cancel the right native timer.
//==============================================================================

static void ForgeTimerFired(void* userData) noexcept;

struct JsTimer {
  JSContext* cx;
  JS::Heap<JSObject*> callback;
  JS::PersistentRootedVector<JS::Value> arguments;

  int jsId{0};
  forge::core::platform::TimerId nativeId{0};
  bool repeat{false};

  explicit JsTimer(JSContext* cx) : cx(cx), arguments(cx) {}
};

class JsTimerRegistry {
 public:
  explicit JsTimerRegistry(forge::core::platform::IoLoop& loop) : loop_(loop) {}

  // Takes ownership of `timer` (already populated with callback/arguments/
  // repeat/delayMs by the caller) and schedules it. Returns the JS-visible
  // id, or -1 if native scheduling (or JS-side registration) failed —
  // `timer` is destroyed in that case, nothing is left registered.
  int Add(forge::core::memory::UniquePtr<JsTimer> timer, uint64_t delayMs) {
    timer->jsId = nextJsId_++;

    JsTimer* raw = timer.Get();

    forge::core::Result<forge::core::platform::TimerId> scheduled =
        loop_.ScheduleTimer(delayMs, raw->repeat, &ForgeTimerFired, raw);

    if (scheduled.HasError()) {
      return -1;
    }

    raw->nativeId = scheduled.Value();

    int jsId = raw->jsId;

    forge::core::Result<bool> inserted = timers_.Insert(jsId, std::move(timer));

    if (inserted.HasError()) {
      // HashMap::Insert's failure path (its internal GrowIfNeeded() running
      // out of memory) returns before ever moving from its V&& argument —
      // so `timer` (this function's local parameter) still owns *raw at
      // this point. That means the native timer scheduled above is now
      // armed against memory that's about to be freed the moment this
      // function returns (timer's destructor runs, since nothing else took
      // ownership) — a real use-after-free the next time it fired. Cancel
      // it first so there's nothing left pointing at *raw once it's gone.
      loop_.CancelTimer(raw->nativeId);
      return -1;
    }

    return jsId;
  }

  void CancelByJsId(int jsId) {
    forge::core::memory::UniquePtr<JsTimer>* found = timers_.Find(jsId);

    if (found == nullptr) {
      return;
    }

    loop_.CancelTimer((*found)->nativeId);
    timers_.Erase(jsId);
  }

  // Called once a one-shot timer's callback has finished running, to
  // release its JsTimer. The native side has already removed the
  // corresponding entry itself (TimerScheduler::PopDue erases one-shots on
  // firing) — this only releases the JS-side wrapper.
  void RemoveFired(int jsId) {
    timers_.Erase(jsId);
  }

  // Traces every live timer's JS callback so the GC does not collect it
  // out from under a still-pending timer. JS::Heap<T> (unlike
  // JS::PersistentRootedVector, which self-registers and needs no manual
  // tracing — that's why `arguments` above needed no attention here) is
  // *not* traced automatically; something owning it must do so explicitly.
  void TraceRoots(JSTracer* trc) {
    for (auto entry : timers_) {
      JS::TraceEdge(trc, &entry.Value()->callback, "forge-timer-callback");
    }
  }

  // Cancels and releases every still-pending timer. Must be called (via
  // Runtime::Shutdown()) while `cx` is still alive and before
  // JS_DestroyContext(cx) — each JsTimer holds a
  // JS::PersistentRootedVector<JS::Value>, which needs to unregister
  // itself from the still-live context when destroyed. Normally run()
  // only stops once every timer is already gone, but it can now also stop
  // early on a genuine I/O error (see Runtime::run()), which is exactly
  // the case this exists for.
  void Clear() {
    for (auto entry : timers_) {
      loop_.CancelTimer(entry.Value()->nativeId);
    }
    timers_.Clear();
  }

 private:
  forge::core::platform::IoLoop& loop_;
  forge::core::HashMap<int, forge::core::memory::UniquePtr<JsTimer>> timers_;
  int nextJsId_{1};
};

static void ForgeTimerFired(void* userData) noexcept {
  auto* timer = static_cast<JsTimer*>(userData);
  JSContext* cx = timer->cx;
  bool repeat = timer->repeat;
  int jsId = timer->jsId;

  // Phase 7.9: after a fatal error nothing else may run, even if the
  // loop delivers more timers in the same RunOnce() batch.
  if (g_stopRequested) {
    return;
  }

  JS::RootedObject callback(cx, timer->callback);
  JS::RootedValue thisValue(cx, JS::UndefinedValue());
  JS::RootedValue rval(cx);

  bool ok = JS::Call(cx, thisValue, callback, timer->arguments,
                     JS::MutableHandleValue(&rval));

  if (!ok) {
    ReportFatalException(cx, "timer callback");
  }

  // Must be last: on a one-shot timer this destroys *timer (via the
  // registry's owning HashMap), so nothing above may touch `timer` again
  // after this — hence copying jsId/repeat out at the top instead of
  // reading timer->... below.
  if (!repeat) {
    GetRuntimeTimers(cx).RemoveFired(jsId);
  }
}

//==============================================================================
// Async fs (Phase 7.5) -- per-operation bookkeeping
//
// See FsAsync.md (frozen design) for the full rationale. Defined here,
// before Runtime, purely so Runtime's own pendingFsReadOps_ member below
// can hold AsyncFsReadOp as a complete type directly -- a HashMap<K,
// UniquePtr<V>> member whose V is only forward-declared would risk
// needing an out-of-line Runtime destructor to sidestep an
// incomplete-type-at-instantiation question this project has no real
// compiler on hand in the sandbox to verify either way; defining the
// type before it's used as a member avoids the question entirely rather
// than guessing at MSVC's template instantiation timing.
//==============================================================================

struct AsyncFsReadOp {
  // Must be the first member: IocpLoop hands GetQueuedCompletionStatus's
  // OVERLAPPED* back as-is, and IoCompletion's own doc comment documents
  // the zero-vtable-call idiom this depends on -- reinterpret_cast'ing
  // that pointer back to AsyncFsReadOp* (see FsAsyncReadOpCompleted
  // below) only works if `completion` is this struct's first member, at
  // offset 0.
  forge::core::platform::IoCompletion completion{};

  // The JSContext this operation was issued on. Forge only ever has one
  // JSContext/realm for the whole process today (see main()), so this is
  // safe the same way JsTimer::cx already is above -- not a
  // multi-context abstraction, just a convenience so the completion
  // callback (a free function, not a method, per IoCompletion::
  // CompletionCallback's plain-function-pointer signature) doesn't need
  // a second parameter threaded through PostCompletion, which has no
  // room for one.
  JSContext* cx{nullptr};

  // GC-rooted independent of whether script keeps a reference to the
  // Promise this call returned (FsAsync.md's Design Goals: a
  // fire-and-forget `fs.writeFile(...);` with no `.then()` must still
  // complete correctly) -- traced by Runtime::TraceRoots, reached via
  // Runtime's pendingFsReadOps_ map below, for as long as this struct is
  // registered there.
  JS::Heap<JSObject*> promise;

  forge::core::Path path;
  bool wantUtf8{false};

  // Exactly one of these two is meaningful once the worker thread has
  // run, selected by wantUtf8 -- mirrors FsReadFileSync's own binary-vs-
  // utf8 branch (Phase 7.3), just executed on a ThreadPool worker
  // instead of inline. Written by the worker thread; read on the main
  // thread afterward, inside FsAsyncReadOpCompleted. The
  // PostCompletion()/GetQueuedCompletionStatus handoff between those two
  // points is the only synchronization protecting these fields -- no
  // Mutex guards them -- see FsAsync.md's Thread Safety section.
  // Placeholder-initialized here (Result<T> has no default constructor)
  // and overwritten by the worker before it calls PostCompletion.
  forge::core::Result<forge::core::Vector<forge::core::u8>> bytesResult;
  forge::core::Result<forge::core::String> textResult;

  explicit AsyncFsReadOp(JSContext* cxIn)
      : cx(cxIn),
        bytesResult(forge::core::Failure{forge::core::Error(forge::core::ErrorCode::Unknown)}),
        textResult(forge::core::Failure{forge::core::Error(forge::core::ErrorCode::Unknown)}) {}
};

// Phase 7.5.4: the remaining six async fs methods
// (writeFile/appendFile/mkdir/rm/exists/stat) share one bookkeeping
// struct rather than getting one each -- unlike fs.readFile's two
// encodings (a genuinely different shape needing its own struct already
// built and proven in 7.5.3), these six only ever need one of three
// possible result shapes (Result<void>, Result<bool>, Result<u64>) and
// one optional input (write/append's data). A `kind` discriminant
// selects which of the three placeholder-initialized Result<T> members
// is authoritative once the worker has run -- the same pattern
// AsyncFsReadOp's own bytesResult/textResult already established, just
// generalized across six operations instead of two encodings of one.
// This keeps four total async-op struct types out of this phase (this
// one plus AsyncFsReadOp) instead of six, without introducing any new
// pattern (e.g. inheritance-based reinterpret_cast tricks) beyond what
// 7.5.3 already proved works.
enum class AsyncFsMiscOpKind {
  Write,
  Append,
  Mkdir,
  MkdirRecursive,
  Rm,
  Exists,
  Stat,
};

struct AsyncFsMiscOp {
  // Must be the first member -- see AsyncFsReadOp's identical comment
  // above; same zero-vtable-call reinterpret_cast idiom applies here.
  forge::core::platform::IoCompletion completion{};

  JSContext* cx{nullptr};
  JS::Heap<JSObject*> promise;
  forge::core::Path path;
  AsyncFsMiscOpKind kind{AsyncFsMiscOpKind::Rm};

  // Only meaningful for Write/Append: the bytes to write, already copied
  // into an owned buffer on the main thread before dispatch (see
  // CopySpanToVector below, and FsAsync.md's Design Goals -- the sync
  // path's ResolveWriteData/AsByteSpan may alias the caller's own JS
  // buffer or ResolveWriteData's own ownedText storage, neither safe to
  // hand across the thread boundary to a worker that runs at some later,
  // arbitrary time).
  forge::core::Vector<forge::core::u8> data;

  // Exactly one of these three is meaningful once the worker thread has
  // run, selected by `kind`. Written by the worker thread; read on the
  // main thread afterward, inside FsAsyncMiscOpCompleted -- same
  // PostCompletion-handoff-is-the-only-synchronization contract as
  // AsyncFsReadOp's own bytesResult/textResult. Placeholder-initialized
  // here (Result<T> has no default constructor).
  forge::core::Result<void> voidResult;
  forge::core::Result<bool> existsResult;
  forge::core::Result<forge::core::u64> statResult;

  explicit AsyncFsMiscOp(JSContext* cxIn)
      : cx(cxIn),
        voidResult(forge::core::Failure{forge::core::Error(forge::core::ErrorCode::Unknown)}),
        existsResult(forge::core::Failure{forge::core::Error(forge::core::ErrorCode::Unknown)}),
        statResult(forge::core::Failure{forge::core::Error(forge::core::ErrorCode::Unknown)}) {}
};

// Sized the same as ThreadingSmokeTest.cpp's own real-machine-confirmed
// 4-worker case -- not tuned against any real workload yet. Benchmarking
// against a real workload is Phase 7.5.6 (see ROADMAP.md), not this
// milestone; this is a reasonable, unvalidated starting point, not a
// claimed-optimal size.
constexpr forge::core::Size kAsyncWorkerThreadCount = 4;

//==============================================================================
// Runtime / GC root tracing
//==============================================================================

static void TraceForgeRoots(JSTracer* trc, void* data);

class Runtime {
 public:
  explicit Runtime(JSContext* cx)
      : cx_(cx), loop_(), timers_(loop_), jobQueue_() {
    JS::SetJobQueue(cx, &jobQueue_);

    // Phase 7.9: track Promises rejected with no handler, so they can be
    // reported (and made fatal) once the microtask queue drains.
    JS::SetPromiseRejectionTrackerCallback(cx, &Runtime::TrackPromiseRejection, this);
  }

  // Must be called exactly once, after construction and before any timer
  // is scheduled or run() is called.
  [[nodiscard]] forge::core::Result<void> Initialize() {
    if (forge::core::Result<void> result = loop_.Initialize(); result.HasError()) {
      return result;
    }

    // Phase 7.5: a small fixed-size pool for async fs (and, later, any
    // other async I/O) dispatch. See kAsyncWorkerThreadCount's own
    // comment above for why this size is a starting point, not a tuned
    // value.
    if (forge::core::Result<void> poolResult =
            workerPool_.Initialize(kAsyncWorkerThreadCount);
        poolResult.HasError()) {
      return poolResult;
    }

    // Passes `this` (not `&timers_`, as before Phase 7.5) so
    // TraceForgeRoots below can reach every GC root this Runtime owns --
    // both the timer registry and, now, every in-flight async fs
    // operation's Promise -- through one Runtime::TraceRoots() method,
    // rather than threading a second pointer through separately.
    JS_AddExtraGCRootsTracer(cx_, TraceForgeRoots, this);
    return {};
  }

  // Drains microtasks, then blocks on the event loop for the next timer
  // or I/O completion, repeating until there is genuinely nothing left
  // pending — replaces the old EventLoop's busy-poll `while (...) { ...;
  // sleep_for(1ms); }` with the loop actually sleeping until something is
  // due (see ROADMAP.md Phase 2).
  //
  // pendingAsyncOps_ (Phase 7.5) is checked alongside the job queue and
  // the I/O loop's own idle state: IoLoop::Empty()'s own doc comment
  // already flags that it "does not yet account for pending async I/O
  // operations" -- without this, a script whose only remaining work is an
  // in-flight async fs read (no timer pending) would look identical to
  // "nothing left to do", and the process would exit before that read
  // ever completes. See FsAsync.md's Design Goals for the full reasoning.
  //
  // Phase 7.9: after every microtask drain, any Promise still rejected
  // without a handler is reported and ends the process with exit code 1
  // (Node's default since v15). The loop also stops at once after any
  // fatal error (g_stopRequested).
  void run() {
    for (;;) {
      jobQueue_.runJobs(cx_);

      if (!g_stopRequested) {
        ReportUnhandledRejections();
      }

      if (g_stopRequested) {
        break;
      }

      if (!JS::HasAnyMicroTasks(cx_) && loop_.Empty() && pendingAsyncOps_ == 0) {
        break;
      }

      forge::core::Result<void> result = loop_.RunOnce();

      if (result.HasError()) {
        fprintf(stderr, "Forge: event loop I/O error (native code %d)\n",
                result.Error().NativeCode());
        break;
      }
    }
  }

  JsTimerRegistry& timers() { return timers_; }

  // Called (via GetRuntime(cx)) by an async operation's dispatch site
  // (Phase 7.5.3 onward) at the moment work is handed to a ThreadPool
  // worker, so run() above doesn't treat the loop as idle while that
  // operation is still in flight. Every call must be paired with exactly
  // one later OnAsyncOperationFinished() call, once that operation's
  // completion has been delivered and its Promise settled.
  void OnAsyncOperationStarted() { ++pendingAsyncOps_; }

  // Mismatched Started()/Finished() calls are a real programmer error,
  // not a tolerated edge case, so this asserts rather than silently
  // wrapping/clamping if the count is already zero -- same "loud failure
  // over silent misbehavior" convention Thread.h's Joinable()/destructor
  // contract already uses elsewhere in this codebase.
  void OnAsyncOperationFinished() {
    FORGE_ASSERT(pendingAsyncOps_ > 0);
    --pendingAsyncOps_;
  }

  forge::core::platform::IoLoop& Loop() { return loop_; }

  // The single shared pool async fs dispatch (7.5.3 onward) submits its
  // worker-thread tasks to. Named generically (not "fs-specific") since
  // FsAsync.md's Extensibility section expects a future async net.* to
  // reuse the identical worker/main-thread split -- this pool is meant to
  // be shared across whatever async I/O features come after this one,
  // not fs-only.
  forge::core::ThreadPool& WorkerPool() { return workerPool_; }

  // Registers a newly-allocated async fs read operation, taking
  // ownership, so (a) TraceRoots below can trace its Promise root for as
  // long as it's in flight, and (b) it stays alive independent of
  // whether script retains any reference to the Promise it returned (see
  // FsAsync.md's "GC-rooted independent of script" Design Goal). Returns
  // a non-owning pointer for the caller's own dispatch use (e.g. handing
  // &op->completion to IoLoop::PostCompletion), or nullptr on OOM growing
  // the registry -- `op` is left untouched in that case (HashMap::Insert
  // doesn't move from its argument on failure, matching
  // JsTimerRegistry::Add()'s own documented OOM behavior above), so the
  // caller's local UniquePtr still owns it and frees it normally when it
  // goes out of scope.
  AsyncFsReadOp* RegisterAsyncFsReadOp(
      forge::core::memory::UniquePtr<AsyncFsReadOp> op) {
    AsyncFsReadOp* raw = op.Get();
    forge::core::Result<bool> inserted =
        pendingFsReadOps_.Insert(raw, std::move(op));

    if (inserted.HasError()) {
      return nullptr;
    }

    return raw;
  }

  // Called from an operation's own completion callback, on the main
  // thread, once its Promise has been settled -- releases (frees) it.
  // Must be the last thing done with `op`; nothing may touch it after
  // this call.
  void UnregisterAsyncFsReadOp(AsyncFsReadOp* op) {
    pendingFsReadOps_.Erase(op);
  }

  // Same Register/Unregister pair as above, for the Phase 7.5.4
  // AsyncFsMiscOp family (writeFile/appendFile/mkdir/rm/exists/stat) --
  // see AsyncFsMiscOp's own comment for why these six share one struct
  // type (and therefore one map) rather than getting one each.
  AsyncFsMiscOp* RegisterAsyncFsMiscOp(
      forge::core::memory::UniquePtr<AsyncFsMiscOp> op) {
    AsyncFsMiscOp* raw = op.Get();
    forge::core::Result<bool> inserted =
        pendingFsMiscOps_.Insert(raw, std::move(op));

    if (inserted.HasError()) {
      return nullptr;
    }

    return raw;
  }

  void UnregisterAsyncFsMiscOp(AsyncFsMiscOp* op) {
    pendingFsMiscOps_.Erase(op);
  }

  // Traces every GC root this Runtime owns: the timer registry's
  // callbacks (unchanged from before Phase 7.5) plus every in-flight
  // async fs operation's Promise, from both pending-op maps -- so a GC
  // running while any fs.* async call is still in flight cannot collect
  // a Promise script no longer references (FsAsync.md's Design Goals).
  void TraceRoots(JSTracer* trc) {
    timers_.TraceRoots(trc);

    for (auto entry : pendingFsReadOps_) {
      JS::TraceEdge(trc, &entry.Value()->promise, "forge-async-fs-promise");
    }

    for (auto entry : pendingFsMiscOps_) {
      JS::TraceEdge(trc, &entry.Value()->promise, "forge-async-fs-promise");
    }

    for (forge::core::Size i = 0; i < unhandledRejections_.Size(); ++i) {
      JS::TraceEdge(trc, &unhandledRejections_[i]->promise, "forge-unhandled-rejection");
    }
  }

  // Releases every still-pending timer, and stops the worker pool.
  // Call this once, after run() has returned (however it returned), and
  // before JS_DestroyContext(cx) -- see JsTimerRegistry::Clear() for
  // exactly why this ordering matters.
  //
  // workerPool_.Shutdown() blocks until every already-running worker
  // task finishes (joins every thread) -- see ThreadPool::Shutdown's own
  // contract. That matters here specifically: it guarantees no worker
  // thread is still writing to any pending op by the time the two
  // Clear() calls below run, so clearing them here (while cx is still
  // alive) is safe -- releasing each entry's JS::Heap<promise> in a
  // context where JS::Heap's own barrier machinery still has a live cx
  // to work with, rather than deferring that release to Runtime's own
  // implicit destructor, which runs later in main() -- after
  // JS_DestroyContext(cx) -- where doing the same thing would not be.
  //
  // Known, accepted gap this does NOT solve: if run() exited early (e.g.
  // a genuine I/O error) while an async fs operation was still in flight,
  // that operation's Promise is simply dropped here, unresolved and
  // unrejected, rather than ever being settled -- there is no script left
  // running to observe it either way once run() has already exited, so
  // this is a silent-but-harmless loss of that operation's result, not a
  // crash or UB. Not addressed further by this milestone.
  //
  // Phase 7.9: also releases tracked rejections while cx is alive.
  // Microtasks still queued after a fatal error belong to the engine
  // (Phase 7.95) and are dropped with the context.
  void Shutdown() {
    workerPool_.Shutdown();
    pendingFsReadOps_.Clear();
    pendingFsMiscOps_.Clear();
    timers_.Clear();
    unhandledRejections_.Clear();
  }

 private:
  JSContext* cx_;
  forge::core::platform::IoLoop loop_;
  JsTimerRegistry timers_;
  ForgeJobQueue jobQueue_;
  forge::core::ThreadPool workerPool_;
  forge::core::HashMap<AsyncFsReadOp*, forge::core::memory::UniquePtr<AsyncFsReadOp>>
      pendingFsReadOps_;
  forge::core::HashMap<AsyncFsMiscOp*, forge::core::memory::UniquePtr<AsyncFsMiscOp>>
      pendingFsMiscOps_;

  // Phase 7.5: count of in-flight async fs (and, later, any other async
  // I/O) operations dispatched onto ThreadPool but not yet completed.
  // Kept here, on Runtime, rather than on IoLoop -- IoLoop::Empty()'s own
  // doc comment explicitly defers this bookkeeping to the caller instead
  // of mandating it live inside IoLoop, and IoLoop's public API stays
  // frozen per FsAsync.md's Dependencies section (nothing here touches
  // IocpLoop.h/.cpp).
  forge::core::Size pendingAsyncOps_{ 0 };

  // Phase 7.9: Promises currently rejected with no handler, oldest first.
  struct TrackedRejection {
    JS::Heap<JSObject*> promise;
  };

  forge::core::Vector<forge::core::memory::UniquePtr<TrackedRejection>> unhandledRejections_;

  // JS::PromiseRejectionTrackerCallback. Called synchronously by the
  // engine when a Promise is rejected with no handler (Unhandled), and when
  // a handler is attached later (Handled).
  static void TrackPromiseRejection(JSContext* cx, bool mutedErrors, JS::HandleObject promise,
                                    JS::PromiseRejectionHandlingState state, void* data) {
    (void)mutedErrors;
    auto* self = static_cast<Runtime*>(data);
    auto& list = self->unhandledRejections_;

    if (state == JS::PromiseRejectionHandlingState::Handled) {
      for (forge::core::Size i = 0; i < list.Size(); ++i) {
        if (list[i]->promise.unbarrieredGet() == promise.get()) {
          for (forge::core::Size j = i; j + 1 < list.Size(); ++j) {
            list[j] = std::move(list[j + 1]);
          }

          list.PopBack();
          return;
        }
      }

      return;
    }

    forge::core::Result<forge::core::memory::UniquePtr<TrackedRejection>> made =
        forge::core::memory::MakeUnique<TrackedRejection>();

    if (made.HasValue()) {
      made.Value()->promise = promise;

      if (list.PushBack(std::move(made.Value())).HasValue()) {
        return;
      }
    }

    // Out of memory while tracking: fail closed rather than silently lose
    // an unhandled rejection.
    (void)cx;
    fprintf(stderr, "Forge: out of memory while tracking an unhandled Promise rejection\n");
    g_exitCode = 1;
    g_stopRequested = true;
  }

  // Reports every still-unhandled rejection (with the reason's own stack
  // when it is an Error) and makes the process exit with code 1.
  void ReportUnhandledRejections() {
    if (unhandledRejections_.Empty()) {
      return;
    }

    for (forge::core::Size i = 0; i < unhandledRejections_.Size(); ++i) {
      JS::RootedObject promise(cx_, unhandledRejections_[i]->promise);
      JSAutoRealm ar(cx_, promise);

      JS::RootedValue reason(cx_, JS::GetPromiseResult(promise));
      JS::RootedObject stack(cx_);

      if (reason.isObject()) {
        JS::RootedObject reasonObj(cx_, &reason.toObject());
        stack = JS::ExceptionStackOrNull(reasonObj);
      }

      fprintf(stderr, "Uncaught (in promise):\n");
      JS::ExceptionStack exn(cx_, reason, stack);
      PrintExceptionReport(cx_, exn, "unhandled promise rejection");
    }

    unhandledRejections_.Clear();
    g_exitCode = 1;
    g_stopRequested = true;
  }
};

static void TraceForgeRoots(JSTracer* trc, void* data) {
  auto* runtime = static_cast<Runtime*>(data);

  runtime->TraceRoots(trc);
}

Runtime* GetRuntime(JSContext* cx)
{
    JS::RootedObject global(
        cx,
        JS::CurrentGlobalOrNull(cx)
    );

    if (!global) {
        return nullptr;
    }

    JS::Value value =
        JS::GetReservedSlot(global, RuntimeSlot);

    return static_cast<Runtime*>(value.toPrivate());
}

// Thin helper so ForgeTimerFired (a free function, declared above Runtime)
// can reach the registry without needing Runtime's full definition visible
// at its point of use.
static JsTimerRegistry& GetRuntimeTimers(JSContext* cx) {
  return GetRuntime(cx)->timers();
}

static bool Print(JSContext* cx, unsigned argc, JS::Value* vp) {
  JS::CallArgs args = JS::CallArgsFromVp(argc, vp);

  for (unsigned i = 0; i < args.length(); i++) {
    JS::RootedString str(cx, JS::ToString(cx, args[i]));

    if (!str) {
      return false;
    }

    JS::UniqueChars bytes = JS_EncodeStringToUTF8(cx, str);

    if (!bytes) {
      return false;
    }

    printf("%s", bytes.get());

    if (i + 1 < args.length()) {
      printf(" ");
    }
  }

  printf("\n");

  args.rval().setUndefined();
  return true;
}

// Shared by setTimeout and setInterval (Phase 7.9). Before, both
// returned false WITHOUT setting an exception when called with fewer than
// two arguments or a non-object callback. Returning false with nothing
// pending is SpiderMonkey's "uncatchable termination" signal, so
// `setTimeout(fn)` silently killed the whole script. Now:
//   * the delay is optional and defaults to 0, as in browsers and Node;
//   * a non-callable callback throws a catchable TypeError (Forge never
//     evaluates a string callback);
//   * a delay that is negative or NaN becomes 0, and one above
//     2147483647 ms becomes 1, as in Node. Casting a huge double straight
//     to uint64_t was undefined behaviour.
static bool ScheduleJsTimer(JSContext* cx, unsigned argc, JS::Value* vp, bool repeat) {
  JS::CallArgs args = JS::CallArgsFromVp(argc, vp);
  const char* name = repeat ? "setInterval" : "setTimeout";

  if (!args.get(0).isObject() || !JS::IsCallable(&args[0].toObject())) {
    return ThrowNotFunction(cx, repeat ? "setInterval callback" : "setTimeout callback");
  }

  double delay = 0;

  if (args.length() >= 2 && !JS::ToNumber(cx, args[1], &delay)) {
    return false;
  }

  if (!(delay >= 0)) {  // also catches NaN
    delay = 0;
  } else if (delay > 2147483647.0) {
    delay = 1;
  }

  forge::core::Result<forge::core::memory::UniquePtr<JsTimer>> made =
      forge::core::memory::MakeUnique<JsTimer>(cx);

  if (made.HasError()) {
    JS_ReportOutOfMemory(cx);
    return false;
  }

  forge::core::memory::UniquePtr<JsTimer> timer = std::move(made.Value());

  JS::RootedObject callback(cx, &args[0].toObject());
  timer->callback = callback;
  timer->repeat = repeat;

  for (unsigned i = 2; i < args.length(); i++) {
    if (!timer->arguments.append(args[i])) {
      JS_ReportOutOfMemory(cx);
      return false;
    }
  }

  int id = GetRuntime(cx)->timers().Add(std::move(timer), static_cast<uint64_t>(delay));

  if (id < 0) {
    JS_ReportErrorASCII(cx, "%s: failed to schedule timer", name);
    return false;
  }

  args.rval().setInt32(id);

  return true;
}

static bool SetTimeout(JSContext* cx, unsigned argc, JS::Value* vp) {
  return ScheduleJsTimer(cx, argc, vp, /*repeat=*/false);
}
static bool ClearTimeout(JSContext* cx, unsigned argc, JS::Value* vp) {
  JS::CallArgs args = JS::CallArgsFromVp(argc, vp);

  // Phase 7.9: clearTimeout() with no argument is a no-op (browsers and
  // Node); it used to return false with no exception, which terminated
  // the script.
  if (argc < 1) {
    args.rval().setUndefined();
    return true;
  }

  int32_t id;

  if (!JS::ToInt32(cx, args[0], &id)) {
    return false;
  }

  GetRuntime(cx)->timers().CancelByJsId(id);

  args.rval().setUndefined();

  return true;
}
static bool SetInterval(JSContext* cx, unsigned argc, JS::Value* vp) {
  return ScheduleJsTimer(cx, argc, vp, /*repeat=*/true);
}
static bool EnqueueMicrotask(JSContext* cx, JS::HandleObject callback) {
  // Phase 7.95: the engine queue holds (and traces) the callback itself.
  if (!JS::EnqueueMicroTask(cx, JS::ObjectValue(*callback))) {
    if (!JS_IsExceptionPending(cx)) {
      JS_ReportOutOfMemory(cx);
    }
    return false;
  }

  // Undo any JS::JobQueueIsEmpty() from runJobs: the queue is not empty
  // any more (same as the engine's InternalJobQueue).
  JS::JobQueueMayNotBeEmpty(cx);
  return true;
}
static bool QueueMicrotask(JSContext* cx, unsigned argc, JS::Value* vp) {
  JS::CallArgs args = JS::CallArgsFromVp(argc, vp);

  // Phase 7.9: a missing or non-callable argument is a catchable
  // TypeError, not a silent termination.
  if (!args.get(0).isObject() || !JS::IsCallable(&args[0].toObject())) {
    return ThrowNotFunction(cx, "queueMicrotask callback");
  }

  JS::RootedObject callback(cx, &args[0].toObject());

  if (!EnqueueMicrotask(cx, callback)) {
    return false;
  }

  args.rval().setUndefined();

  return true;
}

//==============================================================================
// JS/Native Marshalling Primitives (Phase 7.2) -- real-build-confirmed
// 2026-07-30 (`python mach build` succeeded against this exact file).
//
// Implements JsBindings.md's frozen "Public API" section verbatim (same
// names, parameter types, return types) -- the conventions every future
// JS-visible binding (fs now; net/threads later) is meant to share rather
// than each reinventing its own error/marshalling glue. See JsBindings.md
// for the full design rationale; only implementation-specific notes are
// repeated here.
//
// All six are `static` (internal linkage), matching every other helper
// in this file (Print/SetTimeout/ReportPendingException/etc.) -- none of
// them need to be visible outside this translation unit. None of the six
// are called from any JS-visible entry point yet -- Phase 7.3 (the
// concrete `fs.*Sync` bindings from Fs.md) is what wires them up to a
// JS_DefineFunction. Until then, the self-test suite below (run via
// `forge --self-test`) is what exercises them for real: it gives every
// one of these six a genuine call site with a live JSContext/Realm, so
// none needs an [[maybe_unused]] marker despite nothing script-visible
// calling them yet.
//
// Verification history (per AGENTS.md's "Be Honest"): every SpiderMonkey
// function name/signature used below (JS_NewStringCopyUTF8N,
// JS_IsUint8Array, JS_NewUint8Array, JS_GetUint8ArrayData,
// JS_GetTypedArrayByteLength, JS::IsArrayBufferObject,
// JS::IsDetachedArrayBufferObject, JS::GetArrayBufferData,
// JS::GetArrayBufferByteLength, JS::AutoCheckCannotGC, and everything
// already proven by Print()/SetTimeout()/etc. above) was confirmed by
// reading the real headers under this tree's own
// js/public/{ArrayBuffer.h,experimental/TypedData.h,String.h,
// Exception.h,ErrorReport.h,Conversions.h,PropertyAndElement.h,
// Value.h,GCAPI.h,RootingAPI.h} directly before writing this code -- not
// guessed or inferred from memory. Before the real `mach build`, the
// forge-core-facing logic was separately compiled and run (real
// forge-core headers + a signature-faithful fake JSAPI shim) under
// g++/clang++ with full warnings, ASan+UBSan, and valgrind (16/16
// scenarios, 0 leaks) -- see HISTORY.md's Phase 7.2 entry for the full
// account. The real `mach build` succeeding confirms the SpiderMonkey
// call shapes themselves were right; the self-test suite below adds the
// one thing neither of those passes covered: actually calling these
// functions against a live JSContext and checking their results.
//==============================================================================

// ErrorCode -> error.code string table from JsBindings.md's "Error
// Handling Policy". Exhaustive over every ErrorCode enumerator (verified
// against forge-core/Error.h directly) so adding a new enumerator there
// without updating this switch is a compiler error (no `default:`), not
// a silent "Unknown" fallback for a code that should have had a real
// mapping -- the trailing return below only covers the (impossible in
// practice) case of `code` holding a value outside the enum's defined
// range.
static const char* ErrorCodeToString(forge::core::ErrorCode code) {
  using forge::core::ErrorCode;

  switch (code) {
    case ErrorCode::None:             return "None";
    case ErrorCode::Unknown:          return "Unknown";
    case ErrorCode::InvalidArgument:  return "InvalidArgument";
    case ErrorCode::InvalidOperation: return "InvalidOperation";
    case ErrorCode::NotSupported:     return "NotSupported";
    case ErrorCode::NotImplemented:   return "NotImplemented";
    case ErrorCode::NotFound:         return "NotFound";
    case ErrorCode::AlreadyExists:    return "AlreadyExists";
    case ErrorCode::PermissionDenied: return "PermissionDenied";
    case ErrorCode::Busy:             return "Busy";
    case ErrorCode::Timeout:          return "Timeout";
    case ErrorCode::Cancelled:        return "Cancelled";
    case ErrorCode::EndOfFile:        return "EndOfFile";
    case ErrorCode::IOError:          return "IOError";
    case ErrorCode::OutOfMemory:      return "OutOfMemory";
    case ErrorCode::BufferTooSmall:   return "BufferTooSmall";
    case ErrorCode::Overflow:         return "Overflow";
    case ErrorCode::Underflow:        return "Underflow";
    case ErrorCode::InvalidData:      return "InvalidData";
    case ErrorCode::ParseError:       return "ParseError";
    case ErrorCode::PlatformError:    return "PlatformError";
  }

  return "Unknown";
}

// Throws a JS Error annotated with `.code` (and, when available, `.path`/
// `.syscall`/`.nativeCode`) for a failed Result<T>/Result<void>. Per
// JsBindings.md, OutOfMemory is never routed through here -- callers use
// JS_ReportOutOfMemory directly, matching EnqueueMicrotask/SetTimeout/
// SetInterval above.
static void ThrowJsError(JSContext* cx,
                                          const forge::core::Error& error,
                                          const char* context,
                                          const forge::core::Path* path = nullptr) {
  const char* codeStr = ErrorCodeToString(error.Code());

  // forge::core::Error carries no free-text message (see Error.h -- just
  // Code()/NativeCode()), so the reported message is synthesized from
  // `context` and the mapped code string rather than inventing an
  // Error::Message() this codebase doesn't have.
  JS_ReportErrorUTF8(cx, "%s failed: %s", context, codeStr);

  if (!JS_IsExceptionPending(cx)) {
    // JS_ReportErrorUTF8 itself hit trouble (e.g. OOM formatting the
    // message) and left nothing pending to annotate further.
    return;
  }

  JS::RootedValue excVal(cx);
  if (!JS_GetPendingException(cx, &excVal) || !excVal.isObject()) {
    // Not an Error object we can annotate (shouldn't normally happen for
    // an exception JS_ReportErrorUTF8 itself just created) -- the base
    // message is still thrown, just without the extra properties below.
    return;
  }

  JS::RootedObject excObj(cx, &excVal.toObject());

  // Every JS_SetProperty call below is best-effort: if one fails (e.g.
  // OOM allocating the property string), the base thrown Error --
  // already pending -- is left intact rather than compounding the
  // original failure with a second, unrelated OOM report.
  if (JSString* codeJsStr = JS_NewStringCopyZ(cx, codeStr)) {
    JS::RootedValue codeVal(cx, JS::StringValue(codeJsStr));
    JS_SetProperty(cx, excObj, "code", codeVal);
  }

  if (JSString* syscallJsStr = JS_NewStringCopyZ(cx, context)) {
    JS::RootedValue syscallVal(cx, JS::StringValue(syscallJsStr));
    JS_SetProperty(cx, excObj, "syscall", syscallVal);
  }

  if (path != nullptr) {
    const forge::core::StringView pathView = path->View();
    if (JSString* pathJsStr = JS_NewStringCopyUTF8N(
            cx, JS::UTF8Chars(pathView.Data(), pathView.Size()))) {
      JS::RootedValue pathVal(cx, JS::StringValue(pathJsStr));
      JS_SetProperty(cx, excObj, "path", pathVal);
    }
  }

  if (error.Code() == forge::core::ErrorCode::PlatformError) {
    JS::RootedValue nativeCodeVal(cx, JS::NumberValue(error.NativeCode()));
    JS_SetProperty(cx, excObj, "nativeCode", nativeCodeVal);
  }

  // `excObj` is the very heap object already set as cx's pending
  // exception (JS_GetPendingException returned it, not a copy) -- setting
  // properties on it in place is visible to whatever eventually catches
  // it without needing a second JS_SetPendingException call.
}

// Converts an arbitrary JS value to a forge::core::String via ToString
// semantics (matches JS::ToString(cx, value) + JS_EncodeStringToUTF8,
// the exact pattern Print() above already uses and this sandbox cannot
// re-verify beyond that existing, working precedent).
//
// Design note (documented here per the instruction to record any design
// clarification before implementing it -- this is a behavioral
// clarification of JsBindings.md's contract, not a change to any frozen
// signature): a Result<T> failure returned by this function, ToForgePath,
// or any future sibling never leaves its own exception pending on `cx` --
// JS_ClearPendingException is called internally on every failure path
// where an underlying JS:: call (JS::ToString, JS_EncodeStringToUTF8)
// may already have reported one. This lets every caller uniformly do
// `if (result.HasError()) { ThrowJsError(cx, result.Error(), ...); return
// false; }` on any Result<T> from this module without ever risking two
// exceptions pending at once. JsBindings.md's "Error Handling Policy"
// section is updated alongside this change to state the same thing.
static forge::core::Result<forge::core::String> ToForgeString(JSContext* cx,
                                                        JS::HandleValue value) {
  JS::RootedString jsStr(cx, JS::ToString(cx, value));

  if (!jsStr) {
    JS_ClearPendingException(cx);
    return forge::core::Result<forge::core::String>(
        forge::core::Failure{forge::core::Error(forge::core::ErrorCode::InvalidArgument)});
  }

  JS::UniqueChars utf8 = JS_EncodeStringToUTF8(cx, jsStr);

  if (!utf8) {
    JS_ClearPendingException(cx);
    return forge::core::Result<forge::core::String>(
        forge::core::Failure{forge::core::Error(forge::core::ErrorCode::OutOfMemory)});
  }

  // Treated as a null-terminated C string, same assumption Print() above
  // already relies on (printf("%s", bytes.get())) -- a JS string
  // containing an embedded U+0000 would be silently truncated here. Not
  // a new limitation introduced by this function, just inherited from
  // the same JS_EncodeStringToUTF8-based idiom already in production use
  // in this file.
  return forge::core::String::Create(forge::core::StringView(utf8.get()));
}

// Converts a JS value to a forge::core::Path by first converting it to a
// String (above) and then through Path::Create -- Path never touches the
// OS itself (see Path.md), so this is pure value conversion, no new
// failure modes beyond what ToForgeString/Path::Create already have.
static forge::core::Result<forge::core::Path> ToForgePath(
    JSContext* cx, JS::HandleValue value) {
  forge::core::Result<forge::core::String> str = ToForgeString(cx, value);

  if (str.HasError()) {
    return forge::core::Result<forge::core::Path>(
        forge::core::Failure{str.Error()});
  }

  return forge::core::Path::Create(str.Value().View());
}

// Converts a forge::core::StringView to a new JS string (copies; does not
// take ownership of `text`'s storage, matching every JS_New*StringCopy*
// function's own documented convention in js/String.h, as opposed to
// JS_NewUCString's ownership-transferring convention).
//
// Returns nullptr on OOM. JS_NewStringCopyUTF8N is expected to report its
// own failure (standard JS_New*StringCopy* convention across this
// header), but since that isn't spelled out explicitly in js/String.h's
// comments and this sandbox has no way to compile-and-observe it
// directly, this reports defensively if the call somehow left nothing
// pending -- so a caller can always trust "null return means an
// exception is pending" without needing to know which of the two
// conventions is actually in effect.
static JSString* FromForgeString(JSContext* cx,
                                          forge::core::StringView text) {
  JSString* result = JS_NewStringCopyUTF8N(
      cx, JS::UTF8Chars(text.Data(), text.Size()));

  if (!result && !JS_IsExceptionPending(cx)) {
    JS_ReportOutOfMemory(cx);
  }

  return result;
}

// Creates a new Uint8Array and copies `bytes`'s contents into it. Consumes
// `bytes` (taken by value; released when this function returns, per
// JsBindings.md's documented ownership: the JS engine owns the copy from
// here on, forge-core's Vector<u8> is not retained).
static JSObject* Uint8ArrayFromBytes(JSContext* cx,
                                               forge::core::Vector<forge::core::u8> bytes) {
  const size_t length = static_cast<size_t>(bytes.Size());

  JS::RootedObject array(cx, JS_NewUint8Array(cx, length));

  if (!array) {
    // JS_NewUint8Array reports its own failure (OOM, or a RangeError if
    // `length` exceeds the engine's maximum typed array size).
    return nullptr;
  }

  if (length > 0) {
    JS::AutoCheckCannotGC nogc(cx);
    bool isSharedMemory = false;
    uint8_t* data = JS_GetUint8ArrayData(array, &isSharedMemory, nogc);

    // `array` was just created by JS_NewUint8Array immediately above, so
    // it is guaranteed to be a private, non-shared, non-detached buffer:
    // `data` is non-null and `isSharedMemory` is false here by
    // construction, not by runtime luck.
    memcpy(data, bytes.Data(), length);
  }

  return array;
}

// Reads a JS Uint8Array or ArrayBuffer's bytes without copying into a
// forge-core container -- the returned Span aliases the JS buffer's own
// storage and is valid only for the duration of the call site (per
// JsBindings.md), since a GC can move or (for a resizable/detachable
// buffer) invalidate the underlying storage afterward.
//
// Deliberately narrower than "any ArrayBufferView": JsBindings.md's
// Public API doc comment names exactly "a JS Uint8Array/ArrayBuffer",
// not every typed array element-type variant (Int16Array,
// Float64Array, etc.) -- so a caller passing e.g. a Float64Array gets
// InvalidArgument here rather than this function silently reinterpreting
// its bytes as raw uint8_t, which nothing in the frozen spec asked for.
static forge::core::Result<forge::core::Span<const forge::core::u8>> AsByteSpan(
    JSContext* cx, JS::HandleValue value) {
  using ByteSpan = forge::core::Span<const forge::core::u8>;

  if (!value.isObject()) {
    return forge::core::Result<ByteSpan>(
        forge::core::Failure{forge::core::Error(forge::core::ErrorCode::InvalidArgument)});
  }

  JS::RootedObject obj(cx, &value.toObject());

  if (JS_IsUint8Array(obj)) {
    JS::AutoCheckCannotGC nogc(cx);
    bool isSharedMemory = false;
    uint8_t* data = JS_GetUint8ArrayData(obj, &isSharedMemory, nogc);
    const size_t length = JS_GetTypedArrayByteLength(obj);

    if (isSharedMemory || (length > 0 && data == nullptr)) {
      // A SharedArrayBuffer-backed view could be mutated by another
      // thread concurrently with whatever synchronous I/O the caller is
      // about to do with this span -- not safe to alias directly. A
      // null data pointer with nonzero length means a detached backing
      // buffer -- nothing valid to read.
      return forge::core::Result<ByteSpan>(
          forge::core::Failure{forge::core::Error(forge::core::ErrorCode::InvalidOperation)});
    }

    return forge::core::Result<ByteSpan>(
        ByteSpan(reinterpret_cast<const forge::core::u8*>(data), length));
  }

  if (JS::IsArrayBufferObject(obj)) {
    if (JS::IsDetachedArrayBufferObject(obj)) {
      return forge::core::Result<ByteSpan>(
          forge::core::Failure{forge::core::Error(forge::core::ErrorCode::InvalidOperation)});
    }

    JS::AutoCheckCannotGC nogc(cx);
    bool isSharedMemory = false;
    uint8_t* data = JS::GetArrayBufferData(obj, &isSharedMemory, nogc);
    const size_t length = JS::GetArrayBufferByteLength(obj);

    // isSharedMemory is always false for a plain (non-Shared)
    // ArrayBuffer per js/ArrayBuffer.h's own doc comment on
    // JS::GetArrayBufferData -- checked anyway rather than assumed.
    if (isSharedMemory) {
      return forge::core::Result<ByteSpan>(
          forge::core::Failure{forge::core::Error(forge::core::ErrorCode::InvalidOperation)});
    }

    return forge::core::Result<ByteSpan>(
        ByteSpan(reinterpret_cast<const forge::core::u8*>(data), length));
  }

  return forge::core::Result<ByteSpan>(
      forge::core::Failure{forge::core::Error(forge::core::ErrorCode::InvalidArgument)});
}

//==============================================================================
// Phase 7.2 smoke tests -- run via `forge --self-test` (see main()'s CLI
// dispatch below). Exercises each of the six marshalling helpers above
// against a real, live JSContext/Realm -- the one verification step the
// sandbox this was implemented in genuinely could not do (no real
// js/public build graph available there; see the block comment above and
// HISTORY.md's Phase 7.2 entry for what *was* done there). Not
// JS-visible, not part of Fs.md's surface -- a self-contained internal
// diagnostic for this phase, expected to be complemented (not replaced)
// by real fs.*Sync-driven coverage once Phase 7.3 wires these up.
//==============================================================================

// Evaluates a small JS expression and returns its value. Reuses the exact
// JS::SourceText<mozilla::Utf8Unit>/JS::CompileOptions/JS::Evaluate shape
// main() already uses for real script files below, just against an
// in-memory literal instead of a file -- lets the self-tests construct
// arbitrary JS values (a Symbol, a Uint8Array, an ArrayBuffer, ...) for
// the marshalling helpers to operate on. An expression that throws (e.g.
// `Symbol('x')` fed to something expecting ToString to succeed) leaves
// that exception pending for the caller to handle -- not swallowed here.
static bool EvaluateExpression(JSContext* cx, const char* expr,
                               JS::MutableHandleValue rval) {
  JS::SourceText<mozilla::Utf8Unit> src;

  if (!src.init(cx, expr, strlen(expr), JS::SourceOwnership::Borrowed)) {
    return false;
  }

  JS::CompileOptions opts(cx);
  opts.setFileAndLine("<self-test>", 1);

  return JS::Evaluate(cx, opts, src, rval);
}

static bool SelfTest_ToForgeStringFromLiteral(JSContext* cx) {
  JS::RootedValue val(cx);

  if (!EvaluateExpression(cx, "'hello world'", &val)) {
    fprintf(stderr, "  FAIL ToForgeString(literal): evaluate failed\n");
    JS_ClearPendingException(cx);
    return false;
  }

  forge::core::Result<forge::core::String> result = ToForgeString(cx, val);

  if (result.HasError()) {
    fprintf(stderr, "  FAIL ToForgeString(literal): unexpected error\n");
    return false;
  }
  if (result.Value().View() != forge::core::StringView("hello world")) {
    fprintf(stderr, "  FAIL ToForgeString(literal): content mismatch\n");
    return false;
  }

  return true;
}

static bool SelfTest_ToForgeStringFromNumber(JSContext* cx) {
  JS::RootedValue val(cx);

  if (!EvaluateExpression(cx, "42", &val)) {
    fprintf(stderr, "  FAIL ToForgeString(number): evaluate failed\n");
    JS_ClearPendingException(cx);
    return false;
  }

  forge::core::Result<forge::core::String> result = ToForgeString(cx, val);

  if (result.HasError() || result.Value().View() != forge::core::StringView("42")) {
    fprintf(stderr, "  FAIL ToForgeString(number): expected \"42\"\n");
    return false;
  }

  return true;
}

// JS::ToString (the abstract ToString operation) throws a TypeError for a
// Symbol -- this exercises ToForgeString's failure path and the
// JS_ClearPendingException contract documented in JsBindings.md: after a
// failed Result<T>, no exception should be left pending on `cx`.
static bool SelfTest_ToForgeStringSymbolFails(JSContext* cx) {
  JS::RootedValue val(cx);

  if (!EvaluateExpression(cx, "Symbol('x')", &val)) {
    fprintf(stderr, "  FAIL ToForgeString(symbol): evaluate itself failed\n");
    JS_ClearPendingException(cx);
    return false;
  }

  forge::core::Result<forge::core::String> result = ToForgeString(cx, val);
  bool ok = true;

  if (!result.HasError()) {
    fprintf(stderr,
            "  FAIL ToForgeString(symbol): expected failure (ToString "
            "throws on a Symbol)\n");
    ok = false;
  } else if (result.Error().Code() != forge::core::ErrorCode::InvalidArgument) {
    fprintf(stderr, "  FAIL ToForgeString(symbol): expected InvalidArgument\n");
    ok = false;
  }

  if (JS_IsExceptionPending(cx)) {
    fprintf(stderr,
            "  FAIL ToForgeString(symbol): exception left pending -- "
            "violates the documented JS_ClearPendingException contract\n");
    JS_ClearPendingException(cx);
    ok = false;
  }

  return ok;
}

static bool SelfTest_ToForgePathFromLiteral(JSContext* cx) {
  JS::RootedValue val(cx);

  if (!EvaluateExpression(cx, "'a/b/c.txt'", &val)) {
    fprintf(stderr, "  FAIL ToForgePath(literal): evaluate failed\n");
    JS_ClearPendingException(cx);
    return false;
  }

  forge::core::Result<forge::core::Path> result = ToForgePath(cx, val);

  if (result.HasError()) {
    fprintf(stderr, "  FAIL ToForgePath(literal): unexpected error\n");
    return false;
  }
  if (result.Value().View() != forge::core::StringView("a/b/c.txt")) {
    fprintf(stderr, "  FAIL ToForgePath(literal): content mismatch\n");
    return false;
  }

  return true;
}

static bool SelfTest_FromForgeStringRoundTrip(JSContext* cx) {
  const forge::core::StringView text("round-trip");
  JSString* str = FromForgeString(cx, text);

  if (!str) {
    fprintf(stderr, "  FAIL FromForgeString: unexpected null\n");
    return false;
  }

  bool match = false;
  if (!JS_StringEqualsAscii(cx, str, "round-trip", &match) || !match) {
    fprintf(stderr, "  FAIL FromForgeString: content mismatch\n");
    return false;
  }

  return true;
}

static bool SelfTest_Uint8ArrayFromBytes(JSContext* cx) {
  forge::core::Vector<forge::core::u8> bytes;

  const bool pushOk = !bytes.PushBack(forge::core::u8{10}).HasError() &&
                      !bytes.PushBack(forge::core::u8{20}).HasError() &&
                      !bytes.PushBack(forge::core::u8{30}).HasError();

  if (!pushOk) {
    fprintf(stderr, "  FAIL Uint8ArrayFromBytes: test setup PushBack failed\n");
    return false;
  }

  JS::RootedObject array(cx, Uint8ArrayFromBytes(cx, std::move(bytes)));

  if (!array) {
    fprintf(stderr, "  FAIL Uint8ArrayFromBytes: returned null\n");
    return false;
  }
  if (!JS_IsUint8Array(array)) {
    fprintf(stderr, "  FAIL Uint8ArrayFromBytes: result is not a Uint8Array\n");
    return false;
  }

  JS::AutoCheckCannotGC nogc(cx);
  bool isSharedMemory = false;
  uint8_t* data = JS_GetUint8ArrayData(array, &isSharedMemory, nogc);
  const size_t length = JS_GetTypedArrayByteLength(array);

  if (length != 3 || data[0] != 10 || data[1] != 20 || data[2] != 30) {
    fprintf(stderr, "  FAIL Uint8ArrayFromBytes: content mismatch\n");
    return false;
  }

  return true;
}

static bool SelfTest_AsByteSpanUint8Array(JSContext* cx) {
  JS::RootedValue val(cx);

  if (!EvaluateExpression(cx, "new Uint8Array([10, 20, 30])", &val)) {
    fprintf(stderr, "  FAIL AsByteSpan(Uint8Array): evaluate failed\n");
    JS_ClearPendingException(cx);
    return false;
  }

  forge::core::Result<forge::core::Span<const forge::core::u8>> result =
      AsByteSpan(cx, val);

  if (result.HasError()) {
    fprintf(stderr, "  FAIL AsByteSpan(Uint8Array): unexpected error\n");
    return false;
  }

  const forge::core::Span<const forge::core::u8> span = result.Value();
  if (span.Size() != 3 || span[0] != 10 || span[1] != 20 || span[2] != 30) {
    fprintf(stderr, "  FAIL AsByteSpan(Uint8Array): content mismatch\n");
    return false;
  }

  return true;
}

static bool SelfTest_AsByteSpanArrayBuffer(JSContext* cx) {
  JS::RootedValue val(cx);

  if (!EvaluateExpression(cx, "new ArrayBuffer(4)", &val)) {
    fprintf(stderr, "  FAIL AsByteSpan(ArrayBuffer): evaluate failed\n");
    JS_ClearPendingException(cx);
    return false;
  }

  forge::core::Result<forge::core::Span<const forge::core::u8>> result =
      AsByteSpan(cx, val);

  if (result.HasError() || result.Value().Size() != 4) {
    fprintf(stderr, "  FAIL AsByteSpan(ArrayBuffer): expected a 4-byte span\n");
    return false;
  }

  return true;
}

// AsByteSpan is deliberately narrower than "any ArrayBufferView" (see its
// own comment above) -- a Float64Array must be rejected, not silently
// reinterpreted as raw bytes.
static bool SelfTest_AsByteSpanRejectsOtherTypedArray(JSContext* cx) {
  JS::RootedValue val(cx);

  if (!EvaluateExpression(cx, "new Float64Array(2)", &val)) {
    fprintf(stderr, "  FAIL AsByteSpan(Float64Array): evaluate failed\n");
    JS_ClearPendingException(cx);
    return false;
  }

  forge::core::Result<forge::core::Span<const forge::core::u8>> result =
      AsByteSpan(cx, val);

  if (!result.HasError() ||
      result.Error().Code() != forge::core::ErrorCode::InvalidArgument) {
    fprintf(stderr,
            "  FAIL AsByteSpan(Float64Array): expected InvalidArgument\n");
    return false;
  }

  return true;
}

static bool SelfTest_AsByteSpanRejectsNonObject(JSContext* cx) {
  JS::RootedValue val(cx);

  if (!EvaluateExpression(cx, "5", &val)) {
    fprintf(stderr, "  FAIL AsByteSpan(number): evaluate failed\n");
    JS_ClearPendingException(cx);
    return false;
  }

  forge::core::Result<forge::core::Span<const forge::core::u8>> result =
      AsByteSpan(cx, val);

  if (!result.HasError() ||
      result.Error().Code() != forge::core::ErrorCode::InvalidArgument) {
    fprintf(stderr, "  FAIL AsByteSpan(number): expected InvalidArgument\n");
    return false;
  }

  return true;
}

static bool SelfTest_ThrowJsErrorBasicFields(JSContext* cx) {
  forge::core::Result<forge::core::Path> pathResult =
      forge::core::Path::Create(forge::core::StringView("some/file.txt"));

  if (pathResult.HasError()) {
    fprintf(stderr,
            "  FAIL ThrowJsError(basic): test setup Path::Create failed\n");
    return false;
  }

  const forge::core::Error err(forge::core::ErrorCode::NotFound);
  ThrowJsError(cx, err, "readFileSync", &pathResult.Value());

  if (!JS_IsExceptionPending(cx)) {
    fprintf(stderr, "  FAIL ThrowJsError(basic): no exception pending\n");
    return false;
  }

  JS::RootedValue excVal(cx);
  if (!JS_GetPendingException(cx, &excVal) || !excVal.isObject()) {
    fprintf(stderr,
            "  FAIL ThrowJsError(basic): pending exception is not an object\n");
    JS_ClearPendingException(cx);
    return false;
  }

  JS::RootedObject excObj(cx, &excVal.toObject());
  JS::RootedValue codeVal(cx);
  JS::RootedValue syscallVal(cx);
  JS::RootedValue pathVal(cx);
  bool ok = true;

  if (!JS_GetProperty(cx, excObj, "code", &codeVal) ||
      !JS_GetProperty(cx, excObj, "syscall", &syscallVal) ||
      !JS_GetProperty(cx, excObj, "path", &pathVal)) {
    fprintf(stderr, "  FAIL ThrowJsError(basic): could not read properties\n");
    ok = false;
  } else {
    bool match = false;
    if (!codeVal.isString() ||
        !JS_StringEqualsAscii(cx, codeVal.toString(), "NotFound", &match) ||
        !match) {
      fprintf(stderr, "  FAIL ThrowJsError(basic): .code != \"NotFound\"\n");
      ok = false;
    }

    match = false;
    if (!syscallVal.isString() ||
        !JS_StringEqualsAscii(cx, syscallVal.toString(), "readFileSync",
                              &match) ||
        !match) {
      fprintf(stderr,
              "  FAIL ThrowJsError(basic): .syscall != \"readFileSync\"\n");
      ok = false;
    }

    match = false;
    if (!pathVal.isString() ||
        !JS_StringEqualsAscii(cx, pathVal.toString(), "some/file.txt", &match) ||
        !match) {
      fprintf(stderr,
              "  FAIL ThrowJsError(basic): .path != \"some/file.txt\"\n");
      ok = false;
    }
  }

  JS_ClearPendingException(cx);
  return ok;
}

static bool SelfTest_ThrowJsErrorPlatformErrorNativeCode(JSContext* cx) {
  const forge::core::Error err(forge::core::ErrorCode::PlatformError, 5);
  ThrowJsError(cx, err, "statSync");

  if (!JS_IsExceptionPending(cx)) {
    fprintf(stderr,
            "  FAIL ThrowJsError(PlatformError): no exception pending\n");
    return false;
  }

  JS::RootedValue excVal(cx);
  if (!JS_GetPendingException(cx, &excVal) || !excVal.isObject()) {
    fprintf(stderr,
            "  FAIL ThrowJsError(PlatformError): pending exception is not "
            "an object\n");
    JS_ClearPendingException(cx);
    return false;
  }

  JS::RootedObject excObj(cx, &excVal.toObject());
  JS::RootedValue codeVal(cx);
  JS::RootedValue nativeCodeVal(cx);
  bool ok = true;

  if (!JS_GetProperty(cx, excObj, "code", &codeVal) ||
      !JS_GetProperty(cx, excObj, "nativeCode", &nativeCodeVal)) {
    fprintf(stderr,
            "  FAIL ThrowJsError(PlatformError): could not read properties\n");
    ok = false;
  } else {
    bool match = false;
    if (!codeVal.isString() ||
        !JS_StringEqualsAscii(cx, codeVal.toString(), "PlatformError", &match) ||
        !match) {
      fprintf(stderr,
              "  FAIL ThrowJsError(PlatformError): .code != \"PlatformError\"\n");
      ok = false;
    }

    double nativeCodeNum = 0;
    if (!JS::ToNumber(cx, nativeCodeVal, &nativeCodeNum) || nativeCodeNum != 5) {
      fprintf(stderr,
              "  FAIL ThrowJsError(PlatformError): .nativeCode != 5\n");
      ok = false;
    }
  }

  JS_ClearPendingException(cx);
  return ok;
}

// Runs every case above in sequence, printing PASS/FAIL per case, and
// returns true only if every one passed. Invoked via `forge --self-test`
// (see main()'s CLI dispatch below), which now also runs
// RunFsSmokeTests(cx) (Phase 7.3) afterward and prints one combined
// "ALL PASSED"/"FAILURES ABOVE" summary across both suites -- this
// function used to print that summary itself, but doing so here would
// print a possibly-false "ALL PASSED" before the fs suite (which runs
// after this one returns) has even had a chance to fail.
static bool RunMarshallingSmokeTests(JSContext* cx) {
  struct Case {
    const char* name;
    bool (*fn)(JSContext*);
  };

  const Case cases[] = {
      {"ToForgeString(literal)", SelfTest_ToForgeStringFromLiteral},
      {"ToForgeString(number)", SelfTest_ToForgeStringFromNumber},
      {"ToForgeString(symbol) fails cleanly", SelfTest_ToForgeStringSymbolFails},
      {"ToForgePath(literal)", SelfTest_ToForgePathFromLiteral},
      {"FromForgeString round-trip", SelfTest_FromForgeStringRoundTrip},
      {"Uint8ArrayFromBytes", SelfTest_Uint8ArrayFromBytes},
      {"AsByteSpan(Uint8Array)", SelfTest_AsByteSpanUint8Array},
      {"AsByteSpan(ArrayBuffer)", SelfTest_AsByteSpanArrayBuffer},
      {"AsByteSpan rejects Float64Array", SelfTest_AsByteSpanRejectsOtherTypedArray},
      {"AsByteSpan rejects non-object", SelfTest_AsByteSpanRejectsNonObject},
      {"ThrowJsError basic fields", SelfTest_ThrowJsErrorBasicFields},
      {"ThrowJsError PlatformError nativeCode",
       SelfTest_ThrowJsErrorPlatformErrorNativeCode},
  };

  bool allOk = true;

  for (const Case& c : cases) {
    const bool ok = c.fn(cx);
    printf("[self-test] %-40s %s\n", c.name, ok ? "PASS" : "FAIL");
    allOk = allOk && ok;
  }

  return allOk;
}

//==============================================================================
// Phase 7.9 runtime-safety self-tests -- run via `forge --self-test`.
//
// Each case evaluates a small script that must return the string "ok".
// Exit-code behaviour (uncaught errors and unhandled rejections exit 1)
// cannot be tested inside this process; tests/run-runtime-tests.ps1
// covers it by running forge.exe on the scripts in tests/.
//==============================================================================

static bool ExpectScriptReturnsOk(JSContext* cx, const char* label, const char* script) {
  JS::RootedValue rval(cx);

  if (!EvaluateExpression(cx, script, &rval)) {
    fprintf(stderr, "  FAIL %s: script threw\n", label);
    ReportPendingException(cx, label);
    return false;
  }

  forge::core::Result<forge::core::String> text = ToForgeString(cx, rval);

  if (text.HasError()) {
    JS_ClearPendingException(cx);
    fprintf(stderr, "  FAIL %s: result is not a string\n", label);
    return false;
  }

  if (text.Value().View() != forge::core::StringView("ok")) {
    fprintf(stderr, "  FAIL %s: got '%s'\n", label, text.Value().CStr());
    return false;
  }

  return true;
}

static bool SelfTest_SetTimeoutWithoutDelay(JSContext* cx) {
  return ExpectScriptReturnsOk(cx, "setTimeout(fn) without delay",
      "(() => { const id = setTimeout(() => {}); clearTimeout(id);"
      "  return typeof id === 'number' ? 'ok' : 'id is ' + typeof id; })()");
}

static bool SelfTest_SetTimeoutRejectsNonCallable(JSContext* cx) {
  return ExpectScriptReturnsOk(cx, "setTimeout(non-function) throws TypeError",
      "(() => { try { setTimeout(); return 'no throw'; }"
      "  catch (e) { if (!(e instanceof TypeError)) return 'wrong error: ' + e; }"
      "  try { setInterval('code', 1); return 'no throw (string)'; }"
      "  catch (e) { return e instanceof TypeError ? 'ok' : 'wrong error: ' + e; } })()");
}

static bool SelfTest_ClearTimeoutWithoutArgument(JSContext* cx) {
  return ExpectScriptReturnsOk(cx, "clearTimeout() without argument",
      "(() => { clearTimeout(); clearTimeout(undefined); return 'ok'; })()");
}

static bool SelfTest_QueueMicrotaskRejectsNonCallable(JSContext* cx) {
  return ExpectScriptReturnsOk(cx, "queueMicrotask(1) throws TypeError",
      "(() => { try { queueMicrotask(1); return 'no throw'; }"
      "  catch (e) { return e instanceof TypeError ? 'ok' : 'wrong error: ' + e; } })()");
}

static bool SelfTest_SharedArrayBufferDisabled(JSContext* cx) {
  return ExpectScriptReturnsOk(cx, "SharedArrayBuffer is disabled",
      "typeof SharedArrayBuffer === 'undefined' ? 'ok' : 'SharedArrayBuffer exists'");
}

// Flips the process-wide code-generation policy for the duration of one
// script and restores it afterwards, so the hook is proven installed.
static bool SelfTest_CodeGenerationPolicy(JSContext* cx) {
  const CodeGenerationPolicy saved = g_codeGenPolicy;
  bool ok = true;

  g_codeGenPolicy.allowStrings = true;
  ok = ExpectScriptReturnsOk(cx, "eval allowed by default",
           "eval('1 + 1') === 2 ? 'ok' : 'eval returned the wrong value'") && ok;

  g_codeGenPolicy.allowStrings = false;
  ok = ExpectScriptReturnsOk(cx, "eval/new Function blocked by policy",
           "(() => { try { eval('1'); return 'eval ran'; } catch (e) {"
           "    if (!(e instanceof EvalError)) return 'wrong error: ' + e; }"
           "  try { new Function('return 1'); return 'Function ran'; } catch (e) {"
           "    return e instanceof EvalError ? 'ok' : 'wrong error: ' + e; } })()") && ok;

  g_codeGenPolicy = saved;
  g_codeGenPolicy.allowWasm = false;
  ok = ExpectScriptReturnsOk(cx, "WebAssembly blocked by policy",
           "(() => { try { new WebAssembly.Module(new Uint8Array([0,97,115,109,1,0,0,0]));"
           "  return 'compiled'; } catch (e) { return 'ok'; } })()") && ok;

  g_codeGenPolicy = saved;
  return ok;
}

static bool RunRuntimeSafetySmokeTests(JSContext* cx) {
  struct Case {
    const char* name;
    bool (*fn)(JSContext*);
  };

  const Case cases[] = {
      {"setTimeout(fn) without delay", SelfTest_SetTimeoutWithoutDelay},
      {"setTimeout(non-function) TypeError", SelfTest_SetTimeoutRejectsNonCallable},
      {"clearTimeout() without argument", SelfTest_ClearTimeoutWithoutArgument},
      {"queueMicrotask(1) TypeError", SelfTest_QueueMicrotaskRejectsNonCallable},
      {"SharedArrayBuffer disabled", SelfTest_SharedArrayBufferDisabled},
      {"code generation policy hook", SelfTest_CodeGenerationPolicy},
  };

  bool allOk = true;

  for (const Case& c : cases) {
    const bool ok = c.fn(cx);
    printf("[self-test] %-40s %s\n", c.name, ok ? "PASS" : "FAIL");
    allOk = allOk && ok;
  }

  return allOk;
}

//==============================================================================
// fs.*Sync JS Bindings (Phase 7.3) -- implements Fs.md's frozen "Public API"
// verbatim against globalThis.fs, built entirely on Phase 7.2's marshalling
// helpers above and File.h/Path.h's already real-build-confirmed
// synchronous I/O (Phase 3). See Fs.md for the full design rationale
// (Non-Goals, Ownership, Error Handling Policy) -- only implementation-
// specific notes are repeated here.
//
// Verification history (per AGENTS.md's "Be Honest"): JS_NewPlainObject's
// signature was confirmed by reading js/src/jsapi.h directly (see the
// include comment above -- it is not exposed under js/public);
// JSPROP_ENUMERATE was confirmed by reading js/public/PropertyDescriptor.h
// directly. File::Open/Read/Write/Seek/SizeInBytes/Exists/MakeDirectory/
// CreateDirectories/Remove/ReadAllBytes/ReadAllText signatures, and the
// exact Win32-error-to-ErrorCode mapping each depends on
// (TranslateWin32Error in forge-core/platform/Win32Error.cpp), were
// confirmed by reading the real File.h/File.cpp/Win32Error.cpp under this
// tree directly -- not guessed. In particular: ERROR_FILE_NOT_FOUND/
// ERROR_PATH_NOT_FOUND both map to ErrorCode::NotFound, and
// ERROR_ALREADY_EXISTS/ERROR_FILE_EXISTS both map to
// ErrorCode::AlreadyExists -- both facts the self-test suite below relies
// on when asserting a specific thrown `.code`.
//==============================================================================

// Resolves writeFileSync/appendFileSync's `data` argument (a JS string,
// Uint8Array, or ArrayBuffer per Fs.md) to a byte span. For a JS string,
// `ownedText` receives the UTF-8-encoded storage the returned span
// aliases -- the caller must keep `ownedText` alive for exactly as long as
// the returned span is used (same "valid only for the duration of the
// call" contract JsBindings.md documents for AsByteSpan itself, which this
// function defers to unchanged for the Uint8Array/ArrayBuffer case).
static forge::core::Result<forge::core::Span<const forge::core::u8>>
ResolveWriteData(JSContext* cx, JS::HandleValue value,
                  forge::core::String* ownedText) {
  using ByteSpan = forge::core::Span<const forge::core::u8>;

  if (value.isString()) {
    forge::core::Result<forge::core::String> text = ToForgeString(cx, value);

    if (text.HasError()) {
      return forge::core::Result<ByteSpan>(forge::core::Failure{text.Error()});
    }

    *ownedText = std::move(text.Value());
    const forge::core::StringView view = ownedText->View();

    return forge::core::Result<ByteSpan>(ByteSpan(
        reinterpret_cast<const forge::core::u8*>(view.Data()), view.Size()));
  }

  return AsByteSpan(cx, value);
}

// Writes every byte of `data` to `file`, retrying past a short write --
// Fs.md's own documented policy: "File::Write's own policy is 'surface a
// short write as-is, don't retry' ... so the retry loop belongs in this
// binding, not in File itself." Throws via ThrowJsError and returns false
// on the first genuine error, or if a Write() call makes no forward
// progress without reporting one (File::Write's contract doesn't rule
// this out explicitly, so this binding guards against looping forever on
// it).
static bool WriteAllBytes(JSContext* cx, forge::core::File& file,
                          forge::core::Span<const forge::core::u8> data,
                          const char* context, const forge::core::Path* path) {
  forge::core::Size written = 0;

  while (written < data.Size()) {
    const forge::core::Span<const forge::core::u8> remaining =
        data.Subspan(written, data.Size() - written);

    forge::core::Result<forge::core::File::SizeType> result = file.Write(remaining);

    if (result.HasError()) {
      ThrowJsError(cx, result.Error(), context, path);
      return false;
    }

    if (result.Value() == 0) {
      ThrowJsError(cx, forge::core::Error(forge::core::ErrorCode::IOError), context, path);
      return false;
    }

    written += result.Value();
  }

  return true;
}

// fs.readFileSync(path, encoding?) -- Uint8Array via File::ReadAllBytes,
// or a string via File::ReadAllText when encoding === "utf8". Any other
// encoding value is a JS-argument-shape error (not a Result<T> failure),
// so it's thrown directly with code InvalidArgument per Fs.md.
static bool FsReadFileSync(JSContext* cx, unsigned argc, JS::Value* vp) {
  JS::CallArgs args = JS::CallArgsFromVp(argc, vp);

  if (!args.requireAtLeast(cx, "readFileSync", 1)) {
    return false;
  }

  forge::core::Result<forge::core::Path> path = ToForgePath(cx, args[0]);

  if (path.HasError()) {
    ThrowJsError(cx, path.Error(), "readFileSync");
    return false;
  }

  bool wantUtf8 = false;

  if (args.hasDefined(1)) {
    forge::core::Result<forge::core::String> encoding = ToForgeString(cx, args[1]);

    if (encoding.HasError()) {
      ThrowJsError(cx, encoding.Error(), "readFileSync", &path.Value());
      return false;
    }

    if (encoding.Value().View() == forge::core::StringView("utf8")) {
      wantUtf8 = true;
    } else {
      ThrowJsError(cx, forge::core::Error(forge::core::ErrorCode::InvalidArgument),
                   "readFileSync", &path.Value());
      return false;
    }
  }

  if (wantUtf8) {
    forge::core::Result<forge::core::String> text =
        forge::core::File::ReadAllText(path.Value());

    if (text.HasError()) {
      ThrowJsError(cx, text.Error(), "readFileSync", &path.Value());
      return false;
    }

    JSString* result = FromForgeString(cx, text.Value().View());

    if (!result) {
      return false;  // FromForgeString already reported (OOM or pending exception).
    }

    args.rval().setString(result);
    return true;
  }

  forge::core::Result<forge::core::Vector<forge::core::u8>> bytes =
      forge::core::File::ReadAllBytes(path.Value());

  if (bytes.HasError()) {
    ThrowJsError(cx, bytes.Error(), "readFileSync", &path.Value());
    return false;
  }

  JSObject* array = Uint8ArrayFromBytes(cx, std::move(bytes.Value()));

  if (!array) {
    return false;  // Uint8ArrayFromBytes/JS_NewUint8Array already reported.
  }

  args.rval().setObject(*array);
  return true;
}

//==============================================================================
// fs.readFile (Phase 7.5.3) -- the first Promise-returning fs binding,
// per FsAsync.md (frozen design). First native use in this codebase of
// JS::NewPromiseObject/JS::ResolvePromise/JS::RejectPromise
// (js/public/Promise.h) -- confirmed via a direct fetch of the real
// header from this same gecko-dev tree (mozilla/gecko-dev on GitHub),
// mirroring Phase 7.2's JS_NewPlainObject precedent of reading the real
// declaration rather than reconstructing one from memory:
//
//   extern JS_PUBLIC_API JSObject* NewPromiseObject(JSContext* cx,
//                                                    JS::HandleObject executor);
//   extern JS_PUBLIC_API bool ResolvePromise(JSContext* cx,
//                                             JS::HandleObject promiseObj,
//                                             JS::HandleValue resolutionValue);
//   extern JS_PUBLIC_API bool RejectPromise(JSContext* cx,
//                                            JS::HandleObject promiseObj,
//                                            JS::HandleValue rejectionValue);
//
// A null `executor` (used below) means "the only way to resolve or
// reject the returned promise is via the JS::ResolvePromise and
// JS::RejectPromise JSAPI functions" -- exactly this binding's shape.
//==============================================================================

// Rejects `promiseObj` with an Error object carrying the exact same
// shape ThrowJsError already produces for every synchronous fs.*Sync
// failure (.code/.syscall/.path/.nativeCode) -- per FsAsync.md's Error
// Handling Policy ("this phase changes delivery... not the error shape
// itself"). Reuses ThrowJsError directly: it throws (sets cx's pending
// exception), and this then retrieves that exception as a value, clears
// it -- a Promise rejection value must never also be left pending on cx
// -- and hands it to JS::RejectPromise instead of letting it propagate
// as a thrown exception.
static void RejectAsyncFsPromise(JSContext* cx, JS::HandleObject promiseObj,
                                  const forge::core::Error& error,
                                  const char* context,
                                  const forge::core::Path* path = nullptr) {
  ThrowJsError(cx, error, context, path);

  JS::RootedValue rejection(cx);

  if (JS_GetPendingException(cx, &rejection)) {
    JS_ClearPendingException(cx);
  } else {
    // ThrowJsError itself hit trouble formatting the message (its own
    // documented fallback) and left nothing pending to retrieve --
    // reject with undefined rather than leaving the Promise unsettled.
    rejection.setUndefined();
  }

  if (!JS::RejectPromise(cx, promiseObj, rejection)) {
    ReportPendingException(cx, "fs promise rejection");
  }
}

// Rejects `promiseObj` with whatever exception is already pending on cx
// (e.g. FromForgeString/Uint8ArrayFromBytes reporting an OOM or
// RangeError while marshalling a successful worker-thread result into
// JS) -- same reasoning as RejectAsyncFsPromise above: clears the
// pending exception first, since a Promise rejection value must not
// simultaneously be a pending exception.
static void RejectAsyncFsPromiseFromPendingException(JSContext* cx,
                                                       JS::HandleObject promiseObj) {
  JS::RootedValue rejection(cx);

  if (JS_GetPendingException(cx, &rejection)) {
    JS_ClearPendingException(cx);
  } else {
    rejection.setUndefined();
  }

  if (!JS::RejectPromise(cx, promiseObj, rejection)) {
    ReportPendingException(cx, "fs promise rejection");
  }
}

// IoCompletion callback for an in-flight fs.readFile() operation,
// dispatched by IocpLoop::RunOnce() on the main thread once the
// ThreadPool worker that ran the actual File::ReadAllBytes/ReadAllText
// call has posted its completion (see FsReadFile below, and 7.5.1's
// IocpCrossThreadSmokeTest.cpp for real-machine confirmation that a
// cross-thread PostCompletion actually wakes RunOnce()).
//
// `self` is AsyncFsReadOp's own embedded `completion` field --
// reinterpret_cast back to the containing struct relies on `completion`
// being AsyncFsReadOp's first member (see its own comment, above,
// before Runtime).
static void FsAsyncReadOpCompleted(forge::core::platform::IoCompletion* self,
                                    DWORD /*bytesTransferred*/,
                                    ULONG_PTR /*completionKey*/,
                                    bool /*succeeded*/,
                                    DWORD /*errorCode*/) noexcept {
  auto* op = reinterpret_cast<AsyncFsReadOp*>(self);
  JSContext* cx = op->cx;

  JS::RootedObject promiseObj(cx, op->promise);

  if (op->wantUtf8) {
    if (op->textResult.HasError()) {
      RejectAsyncFsPromise(cx, promiseObj, op->textResult.Error(), "readFile", &op->path);
    } else {
      JSString* text = FromForgeString(cx, op->textResult.Value().View());

      if (text) {
        JS::RootedValue value(cx, JS::StringValue(text));

        if (!JS::ResolvePromise(cx, promiseObj, value)) {
          ReportPendingException(cx, "readFile promise resolution");
        }
      } else {
        RejectAsyncFsPromiseFromPendingException(cx, promiseObj);
      }
    }
  } else {
    if (op->bytesResult.HasError()) {
      RejectAsyncFsPromise(cx, promiseObj, op->bytesResult.Error(), "readFile", &op->path);
    } else {
      JSObject* array = Uint8ArrayFromBytes(cx, std::move(op->bytesResult.Value()));

      if (array) {
        JS::RootedValue value(cx, JS::ObjectValue(*array));

        if (!JS::ResolvePromise(cx, promiseObj, value)) {
          ReportPendingException(cx, "readFile promise resolution");
        }
      } else {
        RejectAsyncFsPromiseFromPendingException(cx, promiseObj);
      }
    }
  }

  Runtime* runtime = GetRuntime(cx);
  runtime->OnAsyncOperationFinished();

  // Must be last: this frees *op (via Runtime's owning map) -- nothing
  // above may touch op/self (which aliases the now-freed struct) after
  // this call. promiseObj is a JS::RootedObject holding its own copy of
  // the pointer, so it stays valid through the calls above regardless.
  runtime->UnregisterAsyncFsReadOp(op);
}

// fs.readFile(path, encoding?) -- Promise-returning counterpart to
// readFileSync (Phase 7.3), per FsAsync.md (Phase 7.5, frozen design).
// Argument parsing is identical to readFileSync's above; what differs is
// everything after that: the actual File::ReadAllBytes/ReadAllText call
// runs on a ThreadPool worker thread instead of inline, and the result
// is delivered by resolving/rejecting a Promise from
// FsAsyncReadOpCompleted (above) once that worker's completion is
// dispatched back to the main thread.
static bool FsReadFile(JSContext* cx, unsigned argc, JS::Value* vp) {
  JS::CallArgs args = JS::CallArgsFromVp(argc, vp);

  if (!args.requireAtLeast(cx, "readFile", 1)) {
    return false;
  }

  forge::core::Result<forge::core::Path> path = ToForgePath(cx, args[0]);

  if (path.HasError()) {
    ThrowJsError(cx, path.Error(), "readFile");
    return false;
  }

  bool wantUtf8 = false;

  if (args.hasDefined(1)) {
    forge::core::Result<forge::core::String> encoding = ToForgeString(cx, args[1]);

    if (encoding.HasError()) {
      ThrowJsError(cx, encoding.Error(), "readFile", &path.Value());
      return false;
    }

    if (encoding.Value().View() == forge::core::StringView("utf8")) {
      wantUtf8 = true;
    } else {
      ThrowJsError(cx, forge::core::Error(forge::core::ErrorCode::InvalidArgument),
                   "readFile", &path.Value());
      return false;
    }
  }

  // Create the Promise itself first, with a null executor -- the only
  // way to settle it from here on is JS::ResolvePromise/RejectPromise,
  // called later from FsAsyncReadOpCompleted (see this section's header
  // comment for js/public/Promise.h's exact, real-header-confirmed
  // contract for a null-executor Promise).
  JS::RootedObject promise(cx, JS::NewPromiseObject(cx, nullptr));

  if (!promise) {
    return false;  // NewPromiseObject reports its own failure (OOM).
  }

  forge::core::Result<forge::core::memory::UniquePtr<AsyncFsReadOp>> made =
      forge::core::memory::MakeUnique<AsyncFsReadOp>(cx);

  if (made.HasError()) {
    JS_ReportOutOfMemory(cx);
    return false;
  }

  forge::core::memory::UniquePtr<AsyncFsReadOp> owned = std::move(made.Value());
  owned->promise = promise;
  owned->path = std::move(path.Value());
  owned->wantUtf8 = wantUtf8;
  owned->completion.callback = &FsAsyncReadOpCompleted;

  Runtime* runtime = GetRuntime(cx);

  AsyncFsReadOp* op = runtime->RegisterAsyncFsReadOp(std::move(owned));

  if (!op) {
    JS_ReportOutOfMemory(cx);
    return false;
  }

  forge::core::platform::IoLoop* loop = &runtime->Loop();

  // The worker thread's own callable: touches only op->path/wantUtf8/
  // bytesResult/textResult (forge-core types) and loop (an IoLoop*, only
  // ever to call PostCompletion, documented safe to call from another
  // thread and now real-machine-confirmed -- see 7.5.1/
  // IocpCrossThreadSmokeTest.cpp). Never touches cx, op->promise, or
  // anything else JSAPI-typed -- see FsAsync.md's "thread boundary is
  // absolute" Design Goal.
  forge::core::Result<void> submitted = runtime->WorkerPool().Submit(
      [op, loop]() {
        if (op->wantUtf8) {
          op->textResult = forge::core::File::ReadAllText(op->path);
        } else {
          op->bytesResult = forge::core::File::ReadAllBytes(op->path);
        }

        // Known, accepted gap (not solved by this milestone): if this
        // PostCompletion call itself fails (e.g. the completion port is
        // somehow invalid), there is no way to report that from this
        // thread, and op's Promise never settles -- op also never gets
        // unregistered, and pendingAsyncOps_ never decrements. This is
        // an exceptional, not a routine, failure mode (the same class of
        // "an OS resource call fails for a reason nothing here can
        // recover from" as e.g. Thread::Create failing outright) and is
        // not addressed by this milestone; flagged here rather than
        // silently assumed away.
        (void)loop->PostCompletion(&op->completion, 0, 0);
      });

  if (submitted.HasError()) {
    // Nothing was armed yet -- dispatch itself failed before any worker
    // ever ran -- so it's safe to just unregister and free op, then
    // report synchronously. No Promise has been returned to script in
    // this path (rval is never set), matching every other OOM-style
    // failure in this file.
    runtime->UnregisterAsyncFsReadOp(op);
    ThrowJsError(cx, submitted.Error(), "readFile");
    return false;
  }

  runtime->OnAsyncOperationStarted();

  args.rval().setObject(*promise);
  return true;
}

//==============================================================================
// fs.writeFile / fs.appendFile / fs.mkdir / fs.rm / fs.exists / fs.stat
// (Phase 7.5.4) -- the remaining six Promise-returning fs bindings, per
// FsAsync.md. Share one AsyncFsMiscOp bookkeeping struct (defined above,
// before Runtime) and one dispatch helper (DispatchAsyncFsMiscOp below)
// rather than repeating fs.readFile's whole struct-plus-dispatch shape
// six times -- see AsyncFsMiscOp's own comment for why.
//==============================================================================

// Worker-thread-safe variant of WriteAllBytes (Phase 7.3, above): the
// identical short-write-retry loop and no-forward-progress guard, but
// returns a plain Result<void> instead of calling ThrowJsError -- this
// runs on a ThreadPool worker thread, which must never touch cx (see
// FsAsync.md's "thread boundary is absolute" Design Goal). The
// completion callback (main thread) turns a returned error into a
// rejected Promise via RejectAsyncFsPromise, the same shape ThrowJsError
// itself would have produced.
static forge::core::Result<void> WriteAllBytesNoThrow(
    forge::core::File& file, forge::core::Span<const forge::core::u8> data) {
  forge::core::Size written = 0;

  while (written < data.Size()) {
    const forge::core::Span<const forge::core::u8> remaining =
        data.Subspan(written, data.Size() - written);

    forge::core::Result<forge::core::File::SizeType> result = file.Write(remaining);

    if (result.HasError()) {
      return forge::core::Result<void>(forge::core::Failure{result.Error()});
    }

    if (result.Value() == 0) {
      return forge::core::Result<void>(
          forge::core::Failure{forge::core::Error(forge::core::ErrorCode::IOError)});
    }

    written += result.Value();
  }

  return {};
}

// Copies `data` into a freshly-constructed, owned Vector<u8> -- used by
// the async write/append path to make its own copy of the caller's bytes
// before dispatching to a worker thread. `data` may alias the caller's
// own JS buffer (the Uint8Array/ArrayBuffer case) or ResolveWriteData's
// own `ownedText` storage (the string case) -- neither is safe to hand
// across the thread boundary to a worker that runs at some later,
// arbitrary time (FsAsync.md's Design Goals). Returns Result<void>
// (rather than the Vector itself) so an OOM partway through (Reserve or
// a PushBack) is reported the same way every other allocation failure in
// this file is, via the caller checking .HasError() on the destination
// it already owns.
static forge::core::Result<void> CopySpanToVector(
    forge::core::Span<const forge::core::u8> data,
    forge::core::Vector<forge::core::u8>* out) {
  if (forge::core::Result<void> reserved = out->Reserve(data.Size());
      reserved.HasError()) {
    return reserved;
  }

  for (forge::core::Size i = 0; i < data.Size(); ++i) {
    if (forge::core::Result<void> pushed = out->PushBack(data[i]);
        pushed.HasError()) {
      return pushed;
    }
  }

  return {};
}

// The `context` string ThrowJsError/RejectAsyncFsPromise report for each
// AsyncFsMiscOpKind -- matches the sync bindings' own function-name
// strings (Mkdir/MkdirRecursive both report as "mkdir", same as
// mkdirSync's single reported name regardless of the recursive option).
static const char* AsyncFsMiscOpContextName(AsyncFsMiscOpKind kind) {
  switch (kind) {
    case AsyncFsMiscOpKind::Write:          return "writeFile";
    case AsyncFsMiscOpKind::Append:         return "appendFile";
    case AsyncFsMiscOpKind::Mkdir:          return "mkdir";
    case AsyncFsMiscOpKind::MkdirRecursive: return "mkdir";
    case AsyncFsMiscOpKind::Rm:             return "rm";
    case AsyncFsMiscOpKind::Exists:         return "exists";
    case AsyncFsMiscOpKind::Stat:           return "stat";
  }

  return "fs";
}

// IoCompletion callback for an in-flight AsyncFsMiscOp, dispatched by
// IocpLoop::RunOnce() on the main thread once the ThreadPool worker that
// ran the actual File call has posted its completion -- same shape as
// FsAsyncReadOpCompleted above, just switching on `kind` to know which
// of voidResult/existsResult/statResult is authoritative and how to
// marshal it into JS.
static void FsAsyncMiscOpCompleted(forge::core::platform::IoCompletion* self,
                                    DWORD /*bytesTransferred*/,
                                    ULONG_PTR /*completionKey*/,
                                    bool /*succeeded*/,
                                    DWORD /*errorCode*/) noexcept {
  auto* op = reinterpret_cast<AsyncFsMiscOp*>(self);
  JSContext* cx = op->cx;

  JS::RootedObject promiseObj(cx, op->promise);
  const char* context = AsyncFsMiscOpContextName(op->kind);

  switch (op->kind) {
    case AsyncFsMiscOpKind::Write:
    case AsyncFsMiscOpKind::Append:
    case AsyncFsMiscOpKind::Mkdir:
    case AsyncFsMiscOpKind::MkdirRecursive:
    case AsyncFsMiscOpKind::Rm: {
      if (op->voidResult.HasError()) {
        RejectAsyncFsPromise(cx, promiseObj, op->voidResult.Error(), context, &op->path);
      } else {
        JS::RootedValue value(cx, JS::UndefinedValue());

        if (!JS::ResolvePromise(cx, promiseObj, value)) {
          ReportPendingException(cx, "fs promise resolution");
        }
      }

      break;
    }

    case AsyncFsMiscOpKind::Exists: {
      if (op->existsResult.HasError()) {
        RejectAsyncFsPromise(cx, promiseObj, op->existsResult.Error(), context, &op->path);
      } else {
        JS::RootedValue value(cx, JS::BooleanValue(op->existsResult.Value()));

        if (!JS::ResolvePromise(cx, promiseObj, value)) {
          ReportPendingException(cx, "fs promise resolution");
        }
      }

      break;
    }

    case AsyncFsMiscOpKind::Stat: {
      if (op->statResult.HasError()) {
        RejectAsyncFsPromise(cx, promiseObj, op->statResult.Error(), context, &op->path);
        break;
      }

      JS::RootedObject stats(cx, JS_NewPlainObject(cx));

      if (!stats) {
        RejectAsyncFsPromiseFromPendingException(cx, promiseObj);
        break;
      }

      // Same 2^53-precision caveat as statSync (Phase 7.3) -- not
      // addressed here either, per Fs.md's own accepted "size: number"
      // spec.
      if (!JS_DefineProperty(cx, stats, "size",
                             static_cast<double>(op->statResult.Value()),
                             JSPROP_ENUMERATE)) {
        RejectAsyncFsPromiseFromPendingException(cx, promiseObj);
        break;
      }

      JS::RootedValue value(cx, JS::ObjectValue(*stats));

      if (!JS::ResolvePromise(cx, promiseObj, value)) {
        ReportPendingException(cx, "fs promise resolution");
      }

      break;
    }
  }

  Runtime* runtime = GetRuntime(cx);
  runtime->OnAsyncOperationFinished();

  // Must be last -- see FsAsyncReadOpCompleted's identical comment.
  runtime->UnregisterAsyncFsMiscOp(op);
}

// Shared creation/dispatch path for all six AsyncFsMiscOp-backed
// bindings below: creates the Promise, allocates and registers the
// bookkeeping struct, submits the worker lambda (switching on `kind` to
// run the right File call -- see AsyncFsMiscOp's own comment for why one
// worker lambda covers all six rather than one each), and returns the
// Promise to script. `data` is only meaningful for Write/Append; every
// other kind passes an empty Vector.
static bool DispatchAsyncFsMiscOp(JSContext* cx, JS::CallArgs& args,
                                   forge::core::Path path,
                                   AsyncFsMiscOpKind kind,
                                   forge::core::Vector<forge::core::u8> data,
                                   const char* context) {
  JS::RootedObject promise(cx, JS::NewPromiseObject(cx, nullptr));

  if (!promise) {
    return false;  // NewPromiseObject reports its own failure (OOM).
  }

  forge::core::Result<forge::core::memory::UniquePtr<AsyncFsMiscOp>> made =
      forge::core::memory::MakeUnique<AsyncFsMiscOp>(cx);

  if (made.HasError()) {
    JS_ReportOutOfMemory(cx);
    return false;
  }

  forge::core::memory::UniquePtr<AsyncFsMiscOp> owned = std::move(made.Value());
  owned->promise = promise;
  owned->path = std::move(path);
  owned->kind = kind;
  owned->data = std::move(data);
  owned->completion.callback = &FsAsyncMiscOpCompleted;

  Runtime* runtime = GetRuntime(cx);

  AsyncFsMiscOp* op = runtime->RegisterAsyncFsMiscOp(std::move(owned));

  if (!op) {
    JS_ReportOutOfMemory(cx);
    return false;
  }

  forge::core::platform::IoLoop* loop = &runtime->Loop();

  // Same absolute thread-boundary rule as FsReadFile's own worker lambda
  // above: touches only op->path/kind/data/voidResult/existsResult/
  // statResult (forge-core types) and loop (only to call
  // PostCompletion). Never cx, never op->promise.
  forge::core::Result<void> submitted = runtime->WorkerPool().Submit(
      [op, loop]() {
        switch (op->kind) {
          case AsyncFsMiscOpKind::Write:
          case AsyncFsMiscOpKind::Append: {
            const forge::core::FileMode mode =
                op->kind == AsyncFsMiscOpKind::Write
                    ? forge::core::FileMode::Write
                    : forge::core::FileMode::Append;

            forge::core::Result<forge::core::File> file =
                forge::core::File::Open(op->path, mode);

            if (file.HasError()) {
              op->voidResult = forge::core::Result<void>(forge::core::Failure{file.Error()});
            } else {
              op->voidResult = WriteAllBytesNoThrow(
                  file.Value(),
                  forge::core::Span<const forge::core::u8>(op->data.Data(), op->data.Size()));
            }

            break;
          }

          case AsyncFsMiscOpKind::Mkdir:
            op->voidResult = forge::core::File::MakeDirectory(op->path);
            break;

          case AsyncFsMiscOpKind::MkdirRecursive:
            op->voidResult = forge::core::File::CreateDirectories(op->path);
            break;

          case AsyncFsMiscOpKind::Rm:
            op->voidResult = forge::core::File::Remove(op->path);
            break;

          case AsyncFsMiscOpKind::Exists:
            op->existsResult = forge::core::File::Exists(op->path);
            break;

          case AsyncFsMiscOpKind::Stat: {
            forge::core::Result<forge::core::File> file =
                forge::core::File::Open(op->path, forge::core::FileMode::Read);

            if (file.HasError()) {
              op->statResult =
                  forge::core::Result<forge::core::u64>(forge::core::Failure{file.Error()});
            } else {
              op->statResult = file.Value().SizeInBytes();
              file.Value().Close();
            }

            break;
          }
        }

        // Known, accepted gap -- see FsReadFile's identical comment (a
        // worker-thread PostCompletion failure has no way to be
        // reported, and this op's Promise would never settle).
        (void)loop->PostCompletion(&op->completion, 0, 0);
      });

  if (submitted.HasError()) {
    // Nothing was armed yet -- see FsReadFile's identical comment.
    runtime->UnregisterAsyncFsMiscOp(op);
    ThrowJsError(cx, submitted.Error(), context);
    return false;
  }

  runtime->OnAsyncOperationStarted();

  args.rval().setObject(*promise);
  return true;
}

// fs.writeFile(path, data) / fs.appendFile(path, data) -- Promise-
// returning counterparts to writeFileSync/appendFileSync (Phase 7.3).
// Argument parsing (path, ResolveWriteData) is identical to the sync
// versions'; what's new is copying the resolved bytes into an owned
// Vector<u8> (CopySpanToVector above) before dispatch -- see
// AsyncFsMiscOp's own comment on why the async path cannot alias the
// way the sync path does.
static bool FsWriteOrAppend(JSContext* cx, unsigned argc, JS::Value* vp,
                            AsyncFsMiscOpKind kind, const char* context) {
  JS::CallArgs args = JS::CallArgsFromVp(argc, vp);

  if (!args.requireAtLeast(cx, context, 2)) {
    return false;
  }

  forge::core::Result<forge::core::Path> path = ToForgePath(cx, args[0]);

  if (path.HasError()) {
    ThrowJsError(cx, path.Error(), context);
    return false;
  }

  forge::core::String ownedText;
  forge::core::Result<forge::core::Span<const forge::core::u8>> data =
      ResolveWriteData(cx, args[1], &ownedText);

  if (data.HasError()) {
    ThrowJsError(cx, data.Error(), context, &path.Value());
    return false;
  }

  forge::core::Vector<forge::core::u8> owned;
  forge::core::Result<void> copied = CopySpanToVector(data.Value(), &owned);

  if (copied.HasError()) {
    JS_ReportOutOfMemory(cx);
    return false;
  }

  return DispatchAsyncFsMiscOp(cx, args, std::move(path.Value()), kind,
                                std::move(owned), context);
}

static bool FsWriteFile(JSContext* cx, unsigned argc, JS::Value* vp) {
  return FsWriteOrAppend(cx, argc, vp, AsyncFsMiscOpKind::Write, "writeFile");
}

static bool FsAppendFile(JSContext* cx, unsigned argc, JS::Value* vp) {
  return FsWriteOrAppend(cx, argc, vp, AsyncFsMiscOpKind::Append, "appendFile");
}

// fs.exists(path) -- Promise-returning counterpart to existsSync (Phase
// 7.3). Same semantics/Node-divergence rationale as existsSync above: a
// genuine File::Exists failure rejects rather than resolving false.
static bool FsExists(JSContext* cx, unsigned argc, JS::Value* vp) {
  JS::CallArgs args = JS::CallArgsFromVp(argc, vp);

  if (!args.requireAtLeast(cx, "exists", 1)) {
    return false;
  }

  forge::core::Result<forge::core::Path> path = ToForgePath(cx, args[0]);

  if (path.HasError()) {
    ThrowJsError(cx, path.Error(), "exists");
    return false;
  }

  return DispatchAsyncFsMiscOp(cx, args, std::move(path.Value()),
                                AsyncFsMiscOpKind::Exists,
                                forge::core::Vector<forge::core::u8>(), "exists");
}

// fs.mkdir(path, options?) -- Promise-returning counterpart to mkdirSync
// (Phase 7.3). Same options.recursive parsing and File::MakeDirectory
// vs. File::CreateDirectories split.
static bool FsMkdir(JSContext* cx, unsigned argc, JS::Value* vp) {
  JS::CallArgs args = JS::CallArgsFromVp(argc, vp);

  if (!args.requireAtLeast(cx, "mkdir", 1)) {
    return false;
  }

  forge::core::Result<forge::core::Path> path = ToForgePath(cx, args[0]);

  if (path.HasError()) {
    ThrowJsError(cx, path.Error(), "mkdir");
    return false;
  }

  bool recursive = false;

  if (args.hasDefined(1)) {
    if (!args[1].isObject()) {
      ThrowJsError(cx, forge::core::Error(forge::core::ErrorCode::InvalidArgument),
                   "mkdir", &path.Value());
      return false;
    }

    JS::RootedObject options(cx, &args[1].toObject());
    JS::RootedValue recursiveVal(cx);

    if (!JS_GetProperty(cx, options, "recursive", &recursiveVal)) {
      return false;
    }

    if (!recursiveVal.isUndefined()) {
      recursive = JS::ToBoolean(recursiveVal);
    }
  }

  return DispatchAsyncFsMiscOp(
      cx, args, std::move(path.Value()),
      recursive ? AsyncFsMiscOpKind::MkdirRecursive : AsyncFsMiscOpKind::Mkdir,
      forge::core::Vector<forge::core::u8>(), "mkdir");
}

// fs.rm(path) -- Promise-returning counterpart to rmSync (Phase 7.3).
// File-removal only, same as rmSync, per Fs.md's Non-Goals.
static bool FsRm(JSContext* cx, unsigned argc, JS::Value* vp) {
  JS::CallArgs args = JS::CallArgsFromVp(argc, vp);

  if (!args.requireAtLeast(cx, "rm", 1)) {
    return false;
  }

  forge::core::Result<forge::core::Path> path = ToForgePath(cx, args[0]);

  if (path.HasError()) {
    ThrowJsError(cx, path.Error(), "rm");
    return false;
  }

  return DispatchAsyncFsMiscOp(cx, args, std::move(path.Value()),
                                AsyncFsMiscOpKind::Rm,
                                forge::core::Vector<forge::core::u8>(), "rm");
}

// fs.stat(path) -- Promise-returning counterpart to statSync (Phase
// 7.3). Same { size } shape.
static bool FsStat(JSContext* cx, unsigned argc, JS::Value* vp) {
  JS::CallArgs args = JS::CallArgsFromVp(argc, vp);

  if (!args.requireAtLeast(cx, "stat", 1)) {
    return false;
  }

  forge::core::Result<forge::core::Path> path = ToForgePath(cx, args[0]);

  if (path.HasError()) {
    ThrowJsError(cx, path.Error(), "stat");
    return false;
  }

  return DispatchAsyncFsMiscOp(cx, args, std::move(path.Value()),
                                AsyncFsMiscOpKind::Stat,
                                forge::core::Vector<forge::core::u8>(), "stat");
}

// Shared implementation for writeFileSync/appendFileSync -- identical
// shape per Fs.md ("Same as writeFileSync but File::Open(path,
// FileMode::Append)"), differing only in FileMode and the reported
// function name.
static bool FsWriteOrAppendImpl(JSContext* cx, unsigned argc, JS::Value* vp,
                                forge::core::FileMode mode,
                                const char* context) {
  JS::CallArgs args = JS::CallArgsFromVp(argc, vp);

  if (!args.requireAtLeast(cx, context, 2)) {
    return false;
  }

  forge::core::Result<forge::core::Path> path = ToForgePath(cx, args[0]);

  if (path.HasError()) {
    ThrowJsError(cx, path.Error(), context);
    return false;
  }

  forge::core::String ownedText;
  forge::core::Result<forge::core::Span<const forge::core::u8>> data =
      ResolveWriteData(cx, args[1], &ownedText);

  if (data.HasError()) {
    ThrowJsError(cx, data.Error(), context, &path.Value());
    return false;
  }

  forge::core::Result<forge::core::File> file =
      forge::core::File::Open(path.Value(), mode);

  if (file.HasError()) {
    ThrowJsError(cx, file.Error(), context, &path.Value());
    return false;
  }

  if (!WriteAllBytes(cx, file.Value(), data.Value(), context, &path.Value())) {
    return false;
  }

  args.rval().setUndefined();
  return true;
}

static bool FsWriteFileSync(JSContext* cx, unsigned argc, JS::Value* vp) {
  return FsWriteOrAppendImpl(cx, argc, vp, forge::core::FileMode::Write, "writeFileSync");
}

static bool FsAppendFileSync(JSContext* cx, unsigned argc, JS::Value* vp) {
  return FsWriteOrAppendImpl(cx, argc, vp, forge::core::FileMode::Append, "appendFileSync");
}

// fs.existsSync(path) -- direct File::Exists. Per Fs.md's Error Handling
// Policy (unconditional: "every Result<T>/Result<void> failure throws"),
// a genuine File::Exists failure (e.g. a permission error probing the
// path) still throws here -- a deliberate divergence from Node's own
// fs.existsSync, which swallows every error and returns false. Fs.md's
// own Design Goals explicitly permit divergence from Node's exact
// behavior wherever forge-core's semantics differ, and forge-core draws
// a real distinction between "doesn't exist" (false, not an error) and
// "couldn't tell" (an Error) that this binding preserves rather than
// collapsing the two.
static bool FsExistsSync(JSContext* cx, unsigned argc, JS::Value* vp) {
  JS::CallArgs args = JS::CallArgsFromVp(argc, vp);

  if (!args.requireAtLeast(cx, "existsSync", 1)) {
    return false;
  }

  forge::core::Result<forge::core::Path> path = ToForgePath(cx, args[0]);

  if (path.HasError()) {
    ThrowJsError(cx, path.Error(), "existsSync");
    return false;
  }

  forge::core::Result<bool> exists = forge::core::File::Exists(path.Value());

  if (exists.HasError()) {
    ThrowJsError(cx, exists.Error(), "existsSync", &path.Value());
    return false;
  }

  args.rval().setBoolean(exists.Value());
  return true;
}

// fs.mkdirSync(path, options?) -- options.recursive selects
// File::CreateDirectories ("mkdir -p", tolerates an already-existing
// ancestor -- including the target itself, per its own Result<void>
// bubbling logic in File.cpp) vs. the non-recursive File::MakeDirectory
// (fails with AlreadyExists if the target itself already exists, and
// NotFound if any ancestor is missing).
static bool FsMkdirSync(JSContext* cx, unsigned argc, JS::Value* vp) {
  JS::CallArgs args = JS::CallArgsFromVp(argc, vp);

  if (!args.requireAtLeast(cx, "mkdirSync", 1)) {
    return false;
  }

  forge::core::Result<forge::core::Path> path = ToForgePath(cx, args[0]);

  if (path.HasError()) {
    ThrowJsError(cx, path.Error(), "mkdirSync");
    return false;
  }

  bool recursive = false;

  if (args.hasDefined(1)) {
    if (!args[1].isObject()) {
      ThrowJsError(cx, forge::core::Error(forge::core::ErrorCode::InvalidArgument),
                   "mkdirSync", &path.Value());
      return false;
    }

    JS::RootedObject options(cx, &args[1].toObject());
    JS::RootedValue recursiveVal(cx);

    if (!JS_GetProperty(cx, options, "recursive", &recursiveVal)) {
      return false;
    }

    if (!recursiveVal.isUndefined()) {
      recursive = JS::ToBoolean(recursiveVal);
    }
  }

  forge::core::Result<void> result =
      recursive ? forge::core::File::CreateDirectories(path.Value())
                : forge::core::File::MakeDirectory(path.Value());

  if (result.HasError()) {
    ThrowJsError(cx, result.Error(), "mkdirSync", &path.Value());
    return false;
  }

  args.rval().setUndefined();
  return true;
}

// fs.rmSync(path) -- direct File::Remove (Win32 DeleteFileW). File-removal
// only, per Fs.md's Non-Goals; calling this on a directory surfaces
// whatever File::Remove itself returns for that case (a RemoveDirectoryW
// retry, per File.cpp -- not re-derived here, this binding is a thin
// passthrough).
static bool FsRmSync(JSContext* cx, unsigned argc, JS::Value* vp) {
  JS::CallArgs args = JS::CallArgsFromVp(argc, vp);

  if (!args.requireAtLeast(cx, "rmSync", 1)) {
    return false;
  }

  forge::core::Result<forge::core::Path> path = ToForgePath(cx, args[0]);

  if (path.HasError()) {
    ThrowJsError(cx, path.Error(), "rmSync");
    return false;
  }

  forge::core::Result<void> result = forge::core::File::Remove(path.Value());

  if (result.HasError()) {
    ThrowJsError(cx, result.Error(), "rmSync", &path.Value());
    return false;
  }

  args.rval().setUndefined();
  return true;
}

// fs.statSync(path) -- { size }. Opens the file (FileMode::Read, so a
// missing path throws NotFound via the same path readFileSync's failure
// case uses), reads SizeInBytes(), closes it. Per Fs.md's Non-Goals, size
// is the entire returned shape -- no isFile()/isDirectory()/timestamps
// until forge-core actually supports querying them.
static bool FsStatSync(JSContext* cx, unsigned argc, JS::Value* vp) {
  JS::CallArgs args = JS::CallArgsFromVp(argc, vp);

  if (!args.requireAtLeast(cx, "statSync", 1)) {
    return false;
  }

  forge::core::Result<forge::core::Path> path = ToForgePath(cx, args[0]);

  if (path.HasError()) {
    ThrowJsError(cx, path.Error(), "statSync");
    return false;
  }

  forge::core::Result<forge::core::File> file =
      forge::core::File::Open(path.Value(), forge::core::FileMode::Read);

  if (file.HasError()) {
    ThrowJsError(cx, file.Error(), "statSync", &path.Value());
    return false;
  }

  forge::core::Result<forge::core::u64> size = file.Value().SizeInBytes();

  if (size.HasError()) {
    ThrowJsError(cx, size.Error(), "statSync", &path.Value());
    return false;
  }

  file.Value().Close();

  JS::RootedObject stats(cx, JS_NewPlainObject(cx));

  if (!stats) {
    return false;  // JS_NewPlainObject reports its own failure (OOM).
  }

  // A file larger than 2^53 bytes would lose precision as a JS double --
  // not addressed here, same limitation Fs.md's own spec already accepts
  // ("size: number"), and astronomically outside any real workload this
  // runtime targets.
  if (!JS_DefineProperty(cx, stats, "size", static_cast<double>(size.Value()),
                         JSPROP_ENUMERATE)) {
    return false;
  }

  args.rval().setObject(*stats);
  return true;
}

// Creates globalThis.fs and defines every method above on it, per
// JsBindings.md's naming convention (a namespaced global object, not more
// flat globals). Called once from main(), immediately after the existing
// flat-global JS_DefineFunction calls.
static bool DefineFsNamespace(JSContext* cx, JS::HandleObject global) {
  JS::RootedObject fs(cx, JS_NewPlainObject(cx));

  if (!fs) {
    return false;
  }

  if (!JS_DefineFunction(cx, fs, "readFileSync", FsReadFileSync, 1, 0) ||
      // fs.readFile (Phase 7.5.3) -- the bare name JsBindings.md reserved
      // from the start for the Promise-returning counterpart to
      // readFileSync.
      !JS_DefineFunction(cx, fs, "readFile", FsReadFile, 1, 0) ||
      !JS_DefineFunction(cx, fs, "writeFileSync", FsWriteFileSync, 2, 0) ||
      // fs.writeFile/appendFile/exists/mkdir/rm/stat (Phase 7.5.4) -- the
      // remaining bare Promise-returning names JsBindings.md reserved
      // from the start.
      !JS_DefineFunction(cx, fs, "writeFile", FsWriteFile, 2, 0) ||
      !JS_DefineFunction(cx, fs, "appendFileSync", FsAppendFileSync, 2, 0) ||
      !JS_DefineFunction(cx, fs, "appendFile", FsAppendFile, 2, 0) ||
      !JS_DefineFunction(cx, fs, "existsSync", FsExistsSync, 1, 0) ||
      !JS_DefineFunction(cx, fs, "exists", FsExists, 1, 0) ||
      !JS_DefineFunction(cx, fs, "mkdirSync", FsMkdirSync, 1, 0) ||
      !JS_DefineFunction(cx, fs, "mkdir", FsMkdir, 1, 0) ||
      !JS_DefineFunction(cx, fs, "rmSync", FsRmSync, 1, 0) ||
      !JS_DefineFunction(cx, fs, "rm", FsRm, 1, 0) ||
      !JS_DefineFunction(cx, fs, "statSync", FsStatSync, 1, 0) ||
      !JS_DefineFunction(cx, fs, "stat", FsStat, 1, 0)) {
    return false;
  }

  return JS_DefineProperty(cx, global, "fs", fs, 0);
}

//==============================================================================
// Phase 7.3 smoke tests -- run via `forge --self-test` (see main()'s CLI
// dispatch below), immediately after the Phase 7.2 marshalling suite.
// Exercises every fs.*Sync method above through the real JS-visible
// globalThis.fs surface (each case is a JS snippet run via
// EvaluateExpression, exactly like Phase 7.2's suite above) against real
// files under a scratch directory relative to whatever cwd `forge
// --self-test` is run from. Fulfills Fs.md's own Acceptance Criteria: "at
// least one success path and one failure path per method."
//
// Fixtures are deliberately deterministic, fixed-name paths (no
// Date.now()/Math.random()) so repeated runs don't accumulate garbage --
// each case either truncates its own file on write (FileMode::Write) or
// explicitly tolerates the one idempotency edge case that isn't already
// handled by forge-core itself (File::MakeDirectory's non-recursive
// AlreadyExists on a second run -- see SelfTest_FsMkdirSyncNonRecursive
// below). The scratch directory itself is never removed (fs.rmSync is
// file-removal only, per Fs.md's Non-Goals) -- a harmless, documented
// byproduct of running this suite.
//==============================================================================

static const char* kFsScratchDir = "forge-selftest-fs-scratch";

// Ensures the scratch directory exists, via forge-core directly (not
// through fs.mkdirSync -- that's exercised by its own test cases below,
// and shouldn't be entangled with fixture setup every other case relies
// on).
static bool SelfTest_FsEnsureScratchDir() {
  forge::core::Result<forge::core::Path> dir =
      forge::core::Path::Create(forge::core::StringView(kFsScratchDir));

  if (dir.HasError()) {
    fprintf(stderr, "  FAIL fs scratch dir: Path::Create failed\n");
    return false;
  }

  forge::core::Result<void> created = forge::core::File::CreateDirectories(dir.Value());

  if (created.HasError()) {
    fprintf(stderr, "  FAIL fs scratch dir: CreateDirectories failed\n");
    return false;
  }

  return true;
}

// Shared by every failure-path case below: asserts an exception is
// currently pending on `cx` and that its `.code` property (set by
// ThrowJsError -- see JsBindings.md) equals `expectedCode`. Does not clear
// the exception -- every caller does that itself right after, matching
// Phase 7.2's SelfTest_ThrowJsError* cases above.
static bool CheckPendingErrorCode(JSContext* cx, const char* label,
                                  const char* expectedCode) {
  if (!JS_IsExceptionPending(cx)) {
    fprintf(stderr, "  FAIL %s: no exception pending\n", label);
    return false;
  }

  JS::RootedValue excVal(cx);
  if (!JS_GetPendingException(cx, &excVal) || !excVal.isObject()) {
    fprintf(stderr, "  FAIL %s: pending exception is not an object\n", label);
    return false;
  }

  JS::RootedObject excObj(cx, &excVal.toObject());
  JS::RootedValue codeVal(cx);

  if (!JS_GetProperty(cx, excObj, "code", &codeVal)) {
    fprintf(stderr, "  FAIL %s: could not read .code\n", label);
    return false;
  }

  bool match = false;
  if (!codeVal.isString() ||
      !JS_StringEqualsAscii(cx, codeVal.toString(), expectedCode, &match) || !match) {
    fprintf(stderr, "  FAIL %s: .code != \"%s\"\n", label, expectedCode);
    return false;
  }

  return true;
}

// --- Success paths ---

static bool SelfTest_FsWriteFileSyncStringThenReadBytes(JSContext* cx) {
  JS::RootedValue val(cx);

  const char* expr =
      "(() => {"
      "  fs.writeFileSync('forge-selftest-fs-scratch/roundtrip.txt', 'Hello, Forge!');"
      "  return fs.readFileSync('forge-selftest-fs-scratch/roundtrip.txt');"
      "})()";

  if (!EvaluateExpression(cx, expr, &val)) {
    fprintf(stderr, "  FAIL writeFileSync(string)+readFileSync(bytes): evaluate failed\n");
    JS_ClearPendingException(cx);
    return false;
  }

  if (!val.isObject()) {
    fprintf(stderr, "  FAIL writeFileSync(string)+readFileSync(bytes): result is not an object\n");
    return false;
  }

  JS::RootedObject obj(cx, &val.toObject());

  if (!JS_IsUint8Array(obj)) {
    fprintf(stderr, "  FAIL writeFileSync(string)+readFileSync(bytes): result is not a Uint8Array\n");
    return false;
  }

  JS::AutoCheckCannotGC nogc(cx);
  bool isSharedMemory = false;
  uint8_t* data = JS_GetUint8ArrayData(obj, &isSharedMemory, nogc);
  const size_t length = JS_GetTypedArrayByteLength(obj);

  const char* expected = "Hello, Forge!";
  const size_t expectedLen = strlen(expected);

  if (length != expectedLen || memcmp(data, expected, expectedLen) != 0) {
    fprintf(stderr, "  FAIL writeFileSync(string)+readFileSync(bytes): content mismatch\n");
    return false;
  }

  return true;
}

static bool SelfTest_FsReadFileSyncUtf8Encoding(JSContext* cx) {
  JS::RootedValue val(cx);

  const char* expr =
      "(() => {"
      "  fs.writeFileSync('forge-selftest-fs-scratch/utf8.txt', 'utf8 round trip');"
      "  return fs.readFileSync('forge-selftest-fs-scratch/utf8.txt', 'utf8');"
      "})()";

  if (!EvaluateExpression(cx, expr, &val)) {
    fprintf(stderr, "  FAIL readFileSync(utf8): evaluate failed\n");
    JS_ClearPendingException(cx);
    return false;
  }

  bool match = false;
  if (!val.isString() ||
      !JS_StringEqualsAscii(cx, val.toString(), "utf8 round trip", &match) || !match) {
    fprintf(stderr, "  FAIL readFileSync(utf8): content mismatch\n");
    return false;
  }

  return true;
}

static bool SelfTest_FsWriteFileSyncUint8ArrayData(JSContext* cx) {
  JS::RootedValue val(cx);

  const char* expr =
      "(() => {"
      "  fs.writeFileSync('forge-selftest-fs-scratch/uint8array.txt', new Uint8Array([72, 105, 33]));"
      "  return fs.readFileSync('forge-selftest-fs-scratch/uint8array.txt', 'utf8');"
      "})()";

  if (!EvaluateExpression(cx, expr, &val)) {
    fprintf(stderr, "  FAIL writeFileSync(Uint8Array): evaluate failed\n");
    JS_ClearPendingException(cx);
    return false;
  }

  bool match = false;
  if (!val.isString() ||
      !JS_StringEqualsAscii(cx, val.toString(), "Hi!", &match) || !match) {
    fprintf(stderr, "  FAIL writeFileSync(Uint8Array): content mismatch\n");
    return false;
  }

  return true;
}

static bool SelfTest_FsAppendFileSyncConcatenates(JSContext* cx) {
  JS::RootedValue val(cx);

  const char* expr =
      "(() => {"
      "  fs.writeFileSync('forge-selftest-fs-scratch/append.txt', 'abc');"
      "  fs.appendFileSync('forge-selftest-fs-scratch/append.txt', 'def');"
      "  return fs.readFileSync('forge-selftest-fs-scratch/append.txt', 'utf8');"
      "})()";

  if (!EvaluateExpression(cx, expr, &val)) {
    fprintf(stderr, "  FAIL appendFileSync: evaluate failed\n");
    JS_ClearPendingException(cx);
    return false;
  }

  bool match = false;
  if (!val.isString() ||
      !JS_StringEqualsAscii(cx, val.toString(), "abcdef", &match) || !match) {
    fprintf(stderr, "  FAIL appendFileSync: content mismatch (expected \"abcdef\")\n");
    return false;
  }

  return true;
}

static bool SelfTest_FsExistsSyncTrueForCreatedFile(JSContext* cx) {
  JS::RootedValue val(cx);

  const char* expr =
      "(() => {"
      "  fs.writeFileSync('forge-selftest-fs-scratch/exists.txt', 'x');"
      "  return fs.existsSync('forge-selftest-fs-scratch/exists.txt');"
      "})()";

  if (!EvaluateExpression(cx, expr, &val)) {
    fprintf(stderr, "  FAIL existsSync(true): evaluate failed\n");
    JS_ClearPendingException(cx);
    return false;
  }

  if (!val.isBoolean() || !val.toBoolean()) {
    fprintf(stderr, "  FAIL existsSync(true): expected true\n");
    return false;
  }

  return true;
}

static bool SelfTest_FsExistsSyncFalseForMissingFile(JSContext* cx) {
  JS::RootedValue val(cx);

  if (!EvaluateExpression(
          cx, "fs.existsSync('forge-selftest-fs-scratch/definitely-does-not-exist.txt')",
          &val)) {
    fprintf(stderr, "  FAIL existsSync(false): evaluate failed\n");
    JS_ClearPendingException(cx);
    return false;
  }

  if (!val.isBoolean() || val.toBoolean()) {
    fprintf(stderr, "  FAIL existsSync(false): expected false\n");
    return false;
  }

  return true;
}

// File::MakeDirectory (non-recursive) is not documented as idempotent --
// unlike CreateDirectories, it doesn't tolerate the target itself already
// existing (confirmed via File.cpp: a second CreateDirectoryW on the same
// path returns ERROR_ALREADY_EXISTS, mapped to ErrorCode::AlreadyExists).
// Running this suite twice in a row against the same on-disk scratch
// directory is expected and normal, so this case tolerates that one
// specific, documented outcome rather than treating it as a failure -- the
// postcondition checked below (the directory exists) holds either way.
static bool SelfTest_FsMkdirSyncNonRecursive(JSContext* cx) {
  JS::RootedValue val(cx);

  const char* expr =
      "(() => {"
      "  try {"
      "    fs.mkdirSync('forge-selftest-fs-scratch/mkdir-plain');"
      "  } catch (e) {"
      "    if (e.code !== 'AlreadyExists') { throw e; }"
      "  }"
      "  return fs.existsSync('forge-selftest-fs-scratch/mkdir-plain');"
      "})()";

  if (!EvaluateExpression(cx, expr, &val)) {
    fprintf(stderr, "  FAIL mkdirSync(non-recursive): evaluate failed\n");
    JS_ClearPendingException(cx);
    return false;
  }

  if (!val.isBoolean() || !val.toBoolean()) {
    fprintf(stderr, "  FAIL mkdirSync(non-recursive): directory does not exist afterward\n");
    return false;
  }

  return true;
}

static bool SelfTest_FsMkdirSyncRecursiveNestedDirs(JSContext* cx) {
  JS::RootedValue val(cx);

  const char* expr =
      "(() => {"
      "  fs.mkdirSync('forge-selftest-fs-scratch/mkdir-recursive/a/b/c', { recursive: true });"
      "  return fs.existsSync('forge-selftest-fs-scratch/mkdir-recursive/a/b/c');"
      "})()";

  if (!EvaluateExpression(cx, expr, &val)) {
    fprintf(stderr, "  FAIL mkdirSync(recursive): evaluate failed\n");
    JS_ClearPendingException(cx);
    return false;
  }

  if (!val.isBoolean() || !val.toBoolean()) {
    fprintf(stderr, "  FAIL mkdirSync(recursive): nested directory does not exist afterward\n");
    return false;
  }

  return true;
}

static bool SelfTest_FsRmSyncRemovesFile(JSContext* cx) {
  JS::RootedValue val(cx);

  const char* expr =
      "(() => {"
      "  fs.writeFileSync('forge-selftest-fs-scratch/rm-me.txt', 'temp');"
      "  fs.rmSync('forge-selftest-fs-scratch/rm-me.txt');"
      "  return fs.existsSync('forge-selftest-fs-scratch/rm-me.txt');"
      "})()";

  if (!EvaluateExpression(cx, expr, &val)) {
    fprintf(stderr, "  FAIL rmSync: evaluate failed\n");
    JS_ClearPendingException(cx);
    return false;
  }

  if (!val.isBoolean() || val.toBoolean()) {
    fprintf(stderr, "  FAIL rmSync: file still exists after removal\n");
    return false;
  }

  return true;
}

static bool SelfTest_FsStatSyncReportsSize(JSContext* cx) {
  JS::RootedValue val(cx);

  const char* expr =
      "(() => {"
      "  fs.writeFileSync('forge-selftest-fs-scratch/stat-me.txt', 'twelve bytes');"
      "  return fs.statSync('forge-selftest-fs-scratch/stat-me.txt').size;"
      "})()";

  if (!EvaluateExpression(cx, expr, &val)) {
    fprintf(stderr, "  FAIL statSync: evaluate failed\n");
    JS_ClearPendingException(cx);
    return false;
  }

  double size = 0;
  if (!JS::ToNumber(cx, val, &size) || size != 12) {
    fprintf(stderr, "  FAIL statSync: expected size 12, got a different value\n");
    return false;
  }

  return true;
}

// --- Failure paths (at least one per method, per Fs.md's Acceptance
// Criteria) ---

static bool SelfTest_FsReadFileSyncNotFoundFails(JSContext* cx) {
  JS::RootedValue val(cx);

  if (EvaluateExpression(
          cx, "fs.readFileSync('forge-selftest-fs-scratch/definitely-does-not-exist.txt')",
          &val)) {
    fprintf(stderr, "  FAIL readFileSync(NotFound): expected a thrown error\n");
    return false;
  }

  const bool ok = CheckPendingErrorCode(cx, "readFileSync(NotFound)", "NotFound");
  JS_ClearPendingException(cx);
  return ok;
}

static bool SelfTest_FsWriteFileSyncInvalidDataFails(JSContext* cx) {
  JS::RootedValue val(cx);

  if (EvaluateExpression(
          cx, "fs.writeFileSync('forge-selftest-fs-scratch/invalid-data.txt', 42)", &val)) {
    fprintf(stderr, "  FAIL writeFileSync(InvalidArgument): expected a thrown error\n");
    return false;
  }

  const bool ok = CheckPendingErrorCode(cx, "writeFileSync(InvalidArgument)", "InvalidArgument");
  JS_ClearPendingException(cx);
  return ok;
}

static bool SelfTest_FsAppendFileSyncMissingParentFails(JSContext* cx) {
  JS::RootedValue val(cx);

  if (EvaluateExpression(
          cx,
          "fs.appendFileSync("
          "'forge-selftest-fs-scratch/definitely-missing-parent/child.txt', 'x')",
          &val)) {
    fprintf(stderr, "  FAIL appendFileSync(NotFound): expected a thrown error\n");
    return false;
  }

  const bool ok = CheckPendingErrorCode(cx, "appendFileSync(NotFound)", "NotFound");
  JS_ClearPendingException(cx);
  return ok;
}

// existsSync's own File::Exists call has no easy, portable way to force a
// genuine I/O-level failure from a self-test -- this instead exercises the
// binding's path-conversion failure path (ToForgePath on a Symbol, the
// same mechanism SelfTest_ToForgeStringSymbolFails above already proves
// correct), which is a real failure path through this exact binding, just
// not through File::Exists specifically. Documented here rather than
// silently presented as something it isn't.
static bool SelfTest_FsExistsSyncSymbolPathFails(JSContext* cx) {
  JS::RootedValue val(cx);

  if (EvaluateExpression(cx, "fs.existsSync(Symbol('x'))", &val)) {
    fprintf(stderr, "  FAIL existsSync(InvalidArgument): expected a thrown error\n");
    return false;
  }

  const bool ok = CheckPendingErrorCode(cx, "existsSync(InvalidArgument)", "InvalidArgument");
  JS_ClearPendingException(cx);
  return ok;
}

static bool SelfTest_FsMkdirSyncMissingParentNonRecursiveFails(JSContext* cx) {
  JS::RootedValue val(cx);

  if (EvaluateExpression(
          cx,
          "fs.mkdirSync("
          "'forge-selftest-fs-scratch/definitely-missing-parent/child')",
          &val)) {
    fprintf(stderr, "  FAIL mkdirSync(NotFound): expected a thrown error\n");
    return false;
  }

  const bool ok = CheckPendingErrorCode(cx, "mkdirSync(NotFound)", "NotFound");
  JS_ClearPendingException(cx);
  return ok;
}

static bool SelfTest_FsRmSyncNotFoundFails(JSContext* cx) {
  JS::RootedValue val(cx);

  if (EvaluateExpression(
          cx, "fs.rmSync('forge-selftest-fs-scratch/definitely-does-not-exist.txt')", &val)) {
    fprintf(stderr, "  FAIL rmSync(NotFound): expected a thrown error\n");
    return false;
  }

  const bool ok = CheckPendingErrorCode(cx, "rmSync(NotFound)", "NotFound");
  JS_ClearPendingException(cx);
  return ok;
}

static bool SelfTest_FsStatSyncNotFoundFails(JSContext* cx) {
  JS::RootedValue val(cx);

  if (EvaluateExpression(
          cx, "fs.statSync('forge-selftest-fs-scratch/definitely-does-not-exist.txt')", &val)) {
    fprintf(stderr, "  FAIL statSync(NotFound): expected a thrown error\n");
    return false;
  }

  const bool ok = CheckPendingErrorCode(cx, "statSync(NotFound)", "NotFound");
  JS_ClearPendingException(cx);
  return ok;
}

// Runs every case above in sequence, printing PASS/FAIL per case (same
// convention as RunMarshallingSmokeTests above), and returns true only if
// every one passed -- including the scratch-dir fixture setup itself.
static bool RunFsSmokeTests(JSContext* cx) {
  if (!SelfTest_FsEnsureScratchDir()) {
    printf("[self-test] %-40s %s\n", "fs scratch dir setup", "FAIL");
    return false;
  }

  struct Case {
    const char* name;
    bool (*fn)(JSContext*);
  };

  const Case cases[] = {
      {"fs.writeFileSync(string)+readFileSync(bytes)",
       SelfTest_FsWriteFileSyncStringThenReadBytes},
      {"fs.readFileSync(utf8)", SelfTest_FsReadFileSyncUtf8Encoding},
      {"fs.writeFileSync(Uint8Array)", SelfTest_FsWriteFileSyncUint8ArrayData},
      {"fs.appendFileSync concatenates", SelfTest_FsAppendFileSyncConcatenates},
      {"fs.existsSync(true)", SelfTest_FsExistsSyncTrueForCreatedFile},
      {"fs.existsSync(false)", SelfTest_FsExistsSyncFalseForMissingFile},
      {"fs.mkdirSync(non-recursive)", SelfTest_FsMkdirSyncNonRecursive},
      {"fs.mkdirSync(recursive)", SelfTest_FsMkdirSyncRecursiveNestedDirs},
      {"fs.rmSync removes file", SelfTest_FsRmSyncRemovesFile},
      {"fs.statSync reports size", SelfTest_FsStatSyncReportsSize},
      {"fs.readFileSync fails (NotFound)", SelfTest_FsReadFileSyncNotFoundFails},
      {"fs.writeFileSync fails (InvalidArgument)", SelfTest_FsWriteFileSyncInvalidDataFails},
      {"fs.appendFileSync fails (NotFound)", SelfTest_FsAppendFileSyncMissingParentFails},
      {"fs.existsSync fails (InvalidArgument)", SelfTest_FsExistsSyncSymbolPathFails},
      {"fs.mkdirSync fails (NotFound)", SelfTest_FsMkdirSyncMissingParentNonRecursiveFails},
      {"fs.rmSync fails (NotFound)", SelfTest_FsRmSyncNotFoundFails},
      {"fs.statSync fails (NotFound)", SelfTest_FsStatSyncNotFoundFails},
  };

  bool allOk = true;

  for (const Case& c : cases) {
    const bool ok = c.fn(cx);
    printf("[self-test] %-40s %s\n", c.name, ok ? "PASS" : "FAIL");
    allOk = allOk && ok;
  }

  return allOk;
}

//==============================================================================
// Phase 7.5.5: async fs.* self-tests -- the formal `forge --self-test`
// replacement for manual-test-fs-async.js (Phase 7.5.3/7.5.4's interim,
// non-`--self-test` verification script; see that file's own header
// comment for why it existed and what it covers). Fulfills FsAsync.md's
// own Acceptance Criteria: "Promise-based self-tests should be required
// before considering the phase complete."
//
// Structurally new relative to every self-test suite above:
// RunMarshallingSmokeTests/RunFsSmokeTests never call Runtime::run() --
// each case calls a binding function directly (or evaluates an
// expression that completes synchronously) and inspects a Result<T> or a
// thrown exception immediately afterward. An async fs.* call can only be
// observed by actually letting the event loop run, so these tests instead
// (a) evaluate a script that queues one or more Promise chains, (b) call
// Runtime::run() to actually drive the loop to completion, and only then
// (c) inspect what happened.
//
// Since native code can't itself await a JS Promise, each self-test case
// ends its .then()/.catch() chain by calling a small native recording
// function (AsyncSelfTestRecord below, bound to
// globalThis.__asyncSelfTestRecord only for the duration of this suite --
// not part of Fs.md's JS-visible surface, and not defined outside the
// --self-test code path) with (name, passed, message). Results land in
// g_asyncSelfTestResults, a plain Vector this function points at only
// for the duration of each Evaluate()+run() cycle below -- same
// "main-thread-only, no lifetime overlap with any worker thread" rule
// every other JSAPI touch point in this file already follows, since
// AsyncSelfTestRecord only ever runs as a Promise reaction callback
// during jobQueue_.runJobs(cx_), never from a ThreadPool worker.
//==============================================================================

struct AsyncSelfTestResult {
  forge::core::String name;
  bool passed;
  forge::core::String message;
};

// Set only around each Evaluate()+run() cycle inside RunFsAsyncSmokeTests
// below; nullptr the rest of the time. Main-thread only, exactly like
// every other JSAPI-adjacent piece of global state in this file.
static forge::core::Vector<AsyncSelfTestResult>* g_asyncSelfTestResults = nullptr;

// globalThis.__asyncSelfTestRecord(name, passed, message) -- called from
// the JS side's .then()/.catch() handlers below, never directly by this
// file's C++. Bound with JS_DefineFunction only inside
// RunFsAsyncSmokeTests, so it's simply absent from any real script's
// globalThis.
static bool AsyncSelfTestRecord(JSContext* cx, unsigned argc, JS::Value* vp) {
  JS::CallArgs args = JS::CallArgsFromVp(argc, vp);

  if (!g_asyncSelfTestResults) {
    // Only reachable if a script retained a reference to this function
    // past the Evaluate()+run() cycle that installed it, and called it
    // later -- not expected in practice, but a silent no-op is safer
    // here than dereferencing a null pointer.
    args.rval().setUndefined();
    return true;
  }

  forge::core::Result<forge::core::String> name = ToForgeString(cx, args.get(0));
  if (name.HasError()) {
    return false;
  }

  const bool passed = JS::ToBoolean(args.get(1));

  forge::core::Result<forge::core::String> message = ToForgeString(cx, args.get(2));
  if (message.HasError()) {
    return false;
  }

  forge::core::Result<void> pushed = g_asyncSelfTestResults->PushBack(AsyncSelfTestResult{
      std::move(name.Value()), passed, std::move(message.Value())});

  if (pushed.HasError()) {
    JS_ReportErrorASCII(cx, "__asyncSelfTestRecord: out of memory recording result");
    return false;
  }

  args.rval().setUndefined();
  return true;
}

// Fixture for the Runtime::run() lifetime-isolation probe (Group B)
// below: a file fs.readFile can succeed against, created directly
// through forge-core (not fs.writeFileSync -- that's Phase 7.3's own
// surface, deliberately not entangled with this fixture, same reasoning
// as SelfTest_FsEnsureScratchDir above). Independent of whatever Group A
// leaves behind, so Group B never depends on Group A having run first.
static bool SelfTest_FsAsyncEnsureProbeFixture() {
  forge::core::Result<forge::core::Path> path = forge::core::Path::Create(
      forge::core::StringView("forge-selftest-fs-scratch/selftest-async-lifetime-probe.txt"));

  if (path.HasError()) {
    fprintf(stderr, "  FAIL fs async self-test probe fixture: Path::Create failed\n");
    return false;
  }

  forge::core::Result<forge::core::File> file =
      forge::core::File::Open(path.Value(), forge::core::FileMode::Write);

  if (file.HasError()) {
    fprintf(stderr, "  FAIL fs async self-test probe fixture: File::Open failed\n");
    return false;
  }

  const char* contents = "lifetime-probe";
  const forge::core::Result<void> written = WriteAllBytesNoThrow(
      file.Value(),
      forge::core::Span<const forge::core::u8>(
          reinterpret_cast<const forge::core::u8*>(contents), strlen(contents)));

  if (written.HasError()) {
    fprintf(stderr, "  FAIL fs async self-test probe fixture: write failed\n");
    return false;
  }

  return true;
}

// Group A: exercises all seven fs.* async methods specified in
// FsAsync.md's Public API. The three fs.readFile cases are independent
// of each other (and of everything below), so they fire concurrently,
// same as manual-test-fs-async.js's own structure; the six Phase 7.5.4
// methods are chained through .then() since each one's outcome depends
// on the file state the previous step left behind (see
// manual-test-fs-async.js's own comment for why concurrent calls over the
// same path would be flaky here, deliberately out of FsAsync.md's
// Non-Goals scope).
static const char* kAsyncSelfTestChainScript = R"JS(
(() => {
  const record = (name, passed, message) => {
    __asyncSelfTestRecord(name, !!passed, message === undefined ? "" : String(message));
  };

  const readPath = "forge-selftest-fs-scratch/selftest-async-read.txt";
  const readContents = "phase 7.5.5 self-test";
  fs.writeFileSync(readPath, readContents);

  fs.readFile(readPath, "utf8").then(
    (text) => record("fs.readFile(path, \"utf8\") resolves with correct text",
                      text === readContents,
                      "got " + JSON.stringify(text)),
    (err) => record("fs.readFile(path, \"utf8\") resolves with correct text", false,
                     "unexpectedly rejected: " + JSON.stringify(err))
  );

  fs.readFile(readPath).then(
    (bytes) => record("fs.readFile(path) resolves with a Uint8Array of the right length",
                       bytes instanceof Uint8Array && bytes.length === readContents.length,
                       "got length " + (bytes && bytes.length)),
    (err) => record("fs.readFile(path) resolves with a Uint8Array of the right length", false,
                     "unexpectedly rejected: " + JSON.stringify(err))
  );

  fs.readFile("forge-selftest-fs-scratch/selftest-async-does-not-exist.txt").then(
    () => record("fs.readFile rejects with .code === \"NotFound\" for a missing file", false,
                  "unexpectedly resolved"),
    (err) => record("fs.readFile rejects with .code === \"NotFound\" for a missing file",
                     !!err && err.code === "NotFound",
                     "got " + JSON.stringify(err))
  );

  const miscPath = "forge-selftest-fs-scratch/selftest-async-misc.txt";
  const miscDir = "forge-selftest-fs-scratch/selftest-async-mkdir/nested";

  fs.writeFile(miscPath, "first").then(() => {
    record("fs.writeFile resolves and writes the expected content",
           fs.existsSync(miscPath) && fs.readFileSync(miscPath, "utf8") === "first");
    return fs.appendFile(miscPath, "-second");
  }).then(() => {
    record("fs.appendFile resolves and appends the expected content",
           fs.readFileSync(miscPath, "utf8") === "first-second");
    return fs.stat(miscPath);
  }).then((stats) => {
    const expectedSize = "first-second".length;
    record("fs.stat resolves with the correct size",
           !!stats && stats.size === expectedSize,
           "got " + JSON.stringify(stats));
    return fs.exists(miscPath);
  }).then((existsResult) => {
    record("fs.exists resolves true for a file that exists", existsResult === true);
    return fs.rm(miscPath);
  }).then(() => {
    record("fs.rm resolves and the file is actually gone", !fs.existsSync(miscPath));
    return fs.exists(miscPath);
  }).then((existsResult) => {
    record("fs.exists resolves false for a file that no longer exists", existsResult === false);
    return fs.mkdir(miscDir, { recursive: true });
  }).then(() => {
    // Fs.md's Non-Goals rule out isDirectory()/directory-listing, so
    // existsSync can't directly confirm a *directory* was created --
    // instead, write a real file inside the new nested directory and
    // confirm that succeeds, which is only possible if fs.mkdir
    // (recursive) actually created it.
    let placeholderWorked = false;
    try {
      fs.writeFileSync(miscDir + "/placeholder.txt", "x");
      placeholderWorked = fs.existsSync(miscDir + "/placeholder.txt");
    } catch (e) {
      placeholderWorked = false;
    }
    record("fs.mkdir(recursive) resolves and the nested directory actually exists",
           placeholderWorked);

    // fs.mkdir on an already-existing directory, non-recursive, should
    // reject with AlreadyExists -- mirrors mkdirSync's own documented
    // non-idempotent case (Phase 7.3's SelfTest_FsMkdirSyncNonRecursive).
    return fs.mkdir(miscDir).then(
      () => record(
          "fs.mkdir (non-recursive) on an existing directory rejects with AlreadyExists",
          false, "unexpectedly resolved"),
      (err) => record(
          "fs.mkdir (non-recursive) on an existing directory rejects with AlreadyExists",
          !!err && err.code === "AlreadyExists",
          "got " + JSON.stringify(err))
    );
  }).then(() => {
    record("fs.* chained misc sequence ran to completion without an uncaught rejection", true);
  }).catch((err) => {
    record("fs.* chained misc sequence ran to completion without an uncaught rejection",
           false, "uncaught rejection: " + JSON.stringify(err));
  });
})();
)JS";

// Group B: the Runtime::run() event-loop-lifetime isolation probe. This
// script's ONLY statement, once its body finishes running, is a single
// fs.readFile(...).then(...) call -- no setTimeout/setInterval anywhere,
// nothing else queued. This is the direct, isolated regression test for
// the Phase 7.5.2 pendingAsyncOps_ fix: without it, Runtime::run() would
// see an empty job queue and an empty IoLoop (no timers pending) and
// return immediately, and this .then() callback would never fire -- so
// g_asyncSelfTestResults would still be empty by the time
// RunFsAsyncSmokeTests inspects it below. FsAsync.md's Design Goals
// call this out explicitly as a required case, not just a byproduct of
// the Group A chain above (which always has other pending work
// alongside each individual async call, and so would not by itself catch
// a regression here).
static const char* kAsyncSelfTestLifetimeProbeScript = R"JS(
(() => {
  const record = (name, passed, message) => {
    __asyncSelfTestRecord(name, !!passed, message === undefined ? "" : String(message));
  };

  const probePath = "forge-selftest-fs-scratch/selftest-async-lifetime-probe.txt";
  fs.readFile(probePath, "utf8").then(
    (text) => record("Runtime::run() waits for a lone in-flight fs.readFile (no timers)",
                      text === "lifetime-probe",
                      "got " + JSON.stringify(text)),
    (err) => record("Runtime::run() waits for a lone in-flight fs.readFile (no timers)", false,
                     "unexpectedly rejected: " + JSON.stringify(err))
  );
})();
)JS";

// Runs both groups above, printing each recorded case's PASS/FAIL with
// the same "[self-test] %-40s %s\n" convention every other suite in this
// file uses, and returns true only if every recorded case passed *and*
// both scripts actually produced the number of results expected of them
// (an uncaught exception during Evaluate(), or a chain that silently
// stops partway through without ever reaching its own completion
// record, would otherwise look identical to "zero failures" if this
// function only checked the recorded cases' own `passed` fields).
static bool RunFsAsyncSmokeTests(JSContext* cx, Runtime& runtime) {
  if (!SelfTest_FsEnsureScratchDir()) {
    printf("[self-test] %-40s %s\n", "fs async self-test scratch dir", "FAIL");
    return false;
  }

  if (!SelfTest_FsAsyncEnsureProbeFixture()) {
    printf("[self-test] %-40s %s\n", "fs async self-test probe fixture", "FAIL");
    return false;
  }

  JS::RootedObject global(cx, JS::CurrentGlobalOrNull(cx));

  if (!global || !JS_DefineFunction(cx, global, "__asyncSelfTestRecord",
                                     AsyncSelfTestRecord, 3, 0)) {
    printf("[self-test] %-40s %s\n", "fs async self-test setup", "FAIL");
    return false;
  }

  bool allOk = true;

  // --- Group A: the full chained sequence across all seven methods ---
  {
    forge::core::Vector<AsyncSelfTestResult> results;
    g_asyncSelfTestResults = &results;

    JS::RootedValue rval(cx);
    const bool evaluated = EvaluateExpression(cx, kAsyncSelfTestChainScript, &rval);

    if (!evaluated) {
      ReportPendingException(cx, "fs async self-test (chained sequence)");
      allOk = false;
    } else {
      runtime.run();
    }

    for (forge::core::Size i = 0; i < results.Size(); ++i) {
      const AsyncSelfTestResult& r = results[i];
      printf("[self-test] %-40s %s\n", r.name.CStr(), r.passed ? "PASS" : "FAIL");
      if (!r.passed && r.message.Size() > 0) {
        fprintf(stderr, "  detail: %s\n", r.message.CStr());
      }
      allOk = allOk && r.passed;
    }

    // Expect exactly 12 recorded cases (3 concurrent readFile cases + 9
    // from the chained misc sequence, including its own completion
    // marker) -- a different count means the chain didn't run the way
    // this test expects, independent of whether any individual recorded
    // case passed.
    if (evaluated && results.Size() != 12) {
      printf("[self-test] %-40s %s (expected 12 results, got %llu)\n",
             "fs async self-test (chained sequence) produced all expected results",
             "FAIL", static_cast<unsigned long long>(results.Size()));
      allOk = false;
    }

    g_asyncSelfTestResults = nullptr;
  }

  // --- Group B: Runtime::run() event-loop-lifetime isolation probe ---
  {
    forge::core::Vector<AsyncSelfTestResult> results;
    g_asyncSelfTestResults = &results;

    JS::RootedValue rval(cx);
    const bool evaluated = EvaluateExpression(cx, kAsyncSelfTestLifetimeProbeScript, &rval);

    if (!evaluated) {
      ReportPendingException(cx, "fs async self-test (lifetime probe)");
      allOk = false;
    } else {
      runtime.run();
    }

    const bool probeRan = evaluated && results.Size() == 1 && results[0].passed;
    printf("[self-test] %-40s %s\n",
           "Runtime::run() waits for lone in-flight fs.readFile (no timers)",
           probeRan ? "PASS" : "FAIL");

    if (!probeRan && evaluated && !results.Empty() && results[0].message.Size() > 0) {
      fprintf(stderr, "  detail: %s\n", results[0].message.CStr());
    }

    allOk = allOk && probeRan;
    g_asyncSelfTestResults = nullptr;
  }

  return allOk;
}

static void PrintUsage() {
  printf("Forge JavaScript Runtime\n\n");
  printf("Usage:\n");
  printf("  forge [options] <file.js>   Run a JavaScript file\n");
  printf("  forge --self-test           Run the built-in self-tests\n");
  printf("  forge --version             Show runtime version\n");
  printf("  forge --help                Show this help\n\n");
  printf("Options:\n");
  printf("  --disallow-code-generation-from-strings   Make eval() and new Function() throw\n");
  printf("  --disallow-wasm                           Make WebAssembly compilation throw\n");
  printf("  --async-stacks                            Capture async stack traces (slower)\n");
  printf("  --max-heap-mb=N                           Cap the JS heap at N MB (default: no cap)\n");
}

int main(int argc, char* argv[]) {
  // Phase 7.9 (R22): before anything can load a DLL.
  forge::core::platform::HardenProcess();

  // Runtime options come before the command or script name (Phase 7.9).
  bool asyncStacks = false;
  uint32_t maxHeapBytes = 0xffffffff;  // JSGC_MAX_BYTES default: no cap
  int argIndex = 1;

  for (; argIndex < argc; ++argIndex) {
    const char* arg = argv[argIndex];

    if (strcmp(arg, "--disallow-code-generation-from-strings") == 0) {
      g_codeGenPolicy.allowStrings = false;
    } else if (strcmp(arg, "--disallow-wasm") == 0) {
      g_codeGenPolicy.allowWasm = false;
    } else if (strcmp(arg, "--async-stacks") == 0) {
      asyncStacks = true;
    } else if (strncmp(arg, "--max-heap-mb=", 14) == 0) {
      char* end = nullptr;
      const unsigned long mb = strtoul(arg + 14, &end, 10);

      if (end == arg + 14 || *end != '\0' || mb == 0 || mb > 4095) {
        fprintf(stderr, "Forge: --max-heap-mb must be a whole number from 1 to 4095\n");
        return 2;
      }

      maxHeapBytes = static_cast<uint32_t>(mb) * 1024u * 1024u;
    } else {
      break;
    }
  }

  if (argIndex >= argc) {
    PrintUsage();
    return 0;
  }

  std::string command = argv[argIndex];

  if (command == "--version") {
    printf("Forge v0.1.0\n");
    return 0;
  }

  if (command == "--help") {
    PrintUsage();
    return 0;
  }

  if (command.size() > 2 && command[0] == '-' && command[1] == '-' && command != "--self-test") {
    fprintf(stderr, "Forge: unknown option %s (see forge --help)\n", command.c_str());
    return 2;
  }

  if (!JS_Init()) { // this is where engine start
    return 1;
  }

  // Phase 7.9: real GC settings instead of a fixed 512 MB hard cap.
  // JS_NewContext's argument becomes JSGC_MAX_BYTES, a HARD heap limit
  // (exceeding it is a JS "out of memory" error), not a GC trigger. So,
  // like the SpiderMonkey shell: create the context with the engine's
  // default, then lift the cap (0xffffffff, the engine's own default for
  // JSGC_MAX_BYTES) unless --max-heap-mb asks for one, and let the engine
  // tune its GC heuristics to the machine's physical memory.
  JSContext* cx = JS_NewContext(JS::DefaultHeapMaxBytes); // new context

  if (!cx) {
    return 1;
  }

  JS_SetGCParameter(cx, JSGC_MAX_BYTES, maxHeapBytes);

  if (const forge::core::u64 physicalBytes = forge::core::platform::PhysicalMemoryBytes();
      physicalBytes > 0) {
    const forge::core::u64 physicalMb = physicalBytes / (1024u * 1024u);
    JS_SetGCParametersBasedOnAvailableMemory(
        cx, physicalMb > 0xffffffffu ? 0xffffffffu : static_cast<uint32_t>(physicalMb));
  }

  // Phase 7.9: async stack capture on every Promise reaction is the most
  // likely cause of Forge's slow Promise steps (study 10: ~1.3 us vs
  // ~113 ns in Bun). By default stacks are captured only for debuggees
  // (Forge has no debugger yet, so: never); --async-stacks turns full
  // capture back on. The cost: Error.stack inside async code does not
  // show the async callers. Benchmark both ways before deciding for good.
  JS::ContextOptionsRef(cx).setAsyncStack(true).setAsyncStackCaptureDebuggeeOnly(!asyncStacks);

  // Phase 7.9: every eval/new Function/WebAssembly compilation now passes
  // through ForgeCodeGenerationAllowed, so policy has a place to live.
  JS_SetSecurityCallbacks(cx, &kForgeSecurityCallbacks);

  if (!JS::InitSelfHostedCode(cx)) {
    return 1;
  } // SpiderMonkey's own JavaScript library.
  Runtime runtime(cx);

  forge::core::Result<void> initialized = runtime.Initialize();

  if (initialized.HasError()) {
    fprintf(stderr, "Forge: failed to initialize the event loop (native code %d)\n",
            initialized.Error().NativeCode());
    return 1;
  }

  // Everything from here down that touches `global` (including the
  // JSAutoRealm block below) lives in this outer scope, so `global` — a
  // JS::RootedObject, which unregisters itself from `cx`'s rooting list
  // when destroyed — is destroyed *before* JS_DestroyContext(cx) below,
  // not after. Originally `global` was declared at the same scope as
  // `cx`/`runtime`, so its destructor only ran when main() itself
  // returned, which is after JS_DestroyContext(cx) already ran a few
  // lines down — a real use-after-free (JS::Rooted<T>'s destructor
  // dereferences a now-dangling context pointer to unlink itself). This
  // was a pre-existing bug, not something the event-loop rewrite
  // introduced — it just never had a chance to fire before, because no
  // script had ever run to natural completion through a real
  // `mach`-built forge.exe until the Phase 0 benchmark scripts (see
  // HISTORY.md): hello.js's uncancelled setInterval never lets main()
  // reach this point on its own, so Ctrl+C always killed the process
  // first.
  {
    JS::RealmOptions options;
    // Phase 7.9: SharedArrayBuffer (and Atomics on shared memory) is OFF.
    // Shared memory plus a fine timer is a Spectre-class side channel
    // (study 15); browsers only allow it in cross-origin-isolated pages.
    // It will come back behind an explicit permission later.
    options.creationOptions().setSharedMemoryAndAtomicsEnabled(false);

    JS::RootedObject global(
        cx, JS_NewGlobalObject(cx, &globalClass, nullptr, JS::FireOnNewGlobalHook,
                               options)); // JavaScript universe is born
    if (!global) {
      return 1;
    }
    JS::SetReservedSlot(global, RuntimeSlot, JS::PrivateValue(&runtime));

    {
      JSAutoRealm ar(cx, global); // JSAutoRealm enters that JavaScript world.

      if (!JS::InitRealmStandardClasses(cx)) {
        return 1;
      } // Initialize standard JS class constructors, prototypes, and any top-level functions and constants associated with the standard classes
      JS_DefineFunction(cx, global, "clearTimeout", ClearTimeout, 1, 0);
      if (!JS_DefineFunction(cx, global, "setInterval", SetInterval, 2, 0)) {
        return 1;
      }
      JS_DefineFunction(cx, global, "queueMicrotask", QueueMicrotask, 1, 0);
      if (!JS_DefineFunction(cx, global, "print", Print, 0, 0)) {
        return 1;
      }
      if (!JS_DefineFunction(cx, global, "setTimeout", SetTimeout, 2, 0)) {
        return 1;
      }

      // globalThis.fs (Phase 7.3) -- defined unconditionally, same as
      // every flat global above, so it's available both to `forge
      // --self-test` (RunFsSmokeTests below) and to any real script
      // passed as argv[1].
      if (!DefineFsNamespace(cx, global)) {
        return 1;
      }

      // `forge --self-test` runs the Phase 7.2 marshalling smoke tests
      // (see RunMarshallingSmokeTests above) and the Phase 7.3 fs.*Sync
      // smoke tests (see RunFsSmokeTests above) against this real,
      // already-initialized JSContext/Realm, instead of treating argv[1]
      // as a script path below -- mirrors the --version/--help dispatch
      // above, just handled here (rather than before JS_Init()) because
      // it genuinely needs a live JSContext/Realm to run. Both suites
      // always run, even if the first one fails, so a single invocation
      // surfaces every failure at once rather than stopping at the first.
      if (command == "--self-test") {
        const bool marshallingOk = RunMarshallingSmokeTests(cx);
        const bool runtimeSafetyOk = RunRuntimeSafetySmokeTests(cx);  // Phase 7.9
        const bool fsOk = RunFsSmokeTests(cx);
        // Phase 7.5.5: unlike the two suites above, this one calls
        // runtime.run() (possibly more than once -- see its own comment)
        // to actually drive the event loop, since that's the only way to
        // observe a real async fs.* completion. Must run after fsOk above
        // (not interleaved with it): both suites share the same scratch
        // directory and some of RunFsSmokeTests' *Sync cases are not
        // idempotent across ordering with this suite's own fixture setup.
        const bool fsAsyncOk = RunFsAsyncSmokeTests(cx, runtime);
        const bool ok = marshallingOk && runtimeSafetyOk && fsOk && fsAsyncOk;
        printf("[self-test] %s\n", ok ? "ALL PASSED" : "FAILURES ABOVE");
        runtime.Shutdown();
        return ok ? 0 : 1;
      }

      // Note: argc < 2 is unreachable here — the argc == 1 case already
      // returned at the top of main(), so argc >= 2 is guaranteed by this
      // point. The old dead check has been removed.

      const char* scriptArg = argv[argIndex];

      forge::core::Result<forge::core::Path> scriptPath =
          forge::core::Path::Create(forge::core::StringView(scriptArg));

      if (scriptPath.HasError()) {
        fprintf(stderr, "Forge: invalid script path %s\n", scriptArg);
        return 1;
      }

      forge::core::Result<forge::core::String> source =
          forge::core::File::ReadAllText(scriptPath.Value());

      if (source.HasError()) {
        fprintf(stderr, "Forge: cannot read %s (error code %d)\n", scriptArg,
                static_cast<int>(source.Error().Code()));
        return 1;
      }

      JS::SourceText<mozilla::Utf8Unit> src;

      if (!src.init(cx, source.Value().Data(), source.Value().Size(),
                    JS::SourceOwnership::Borrowed)) {
        return 1;
      }

      JS::RootedValue rval(cx);

      JS::CompileOptions opts(cx);
      opts.setFileAndLine(scriptArg, 1);

      if (!JS::Evaluate(cx, opts, src, &rval)) {
        ReportFatalException(cx, scriptArg);
        // The script may have already called setTimeout/setInterval before
        // failing — release those before cx goes away, same reasoning as
        // the runtime.Shutdown() call after the event loop below.
        runtime.Shutdown();
        return 1;
      }

      if (rval.isNumber()) {
        printf("Result = %f\n", rval.toNumber());
      }
      runtime.run();
    }
  } // end of the `global`-owning scope — global is destroyed here, cx is still alive.

  runtime.Shutdown();
  JS_DestroyContext(cx);
  JS_ShutDown();

  // Phase 7.9: 1 after an uncaught exception or unhandled rejection
  // during the event loop (it used to be 0 no matter what).
  return g_exitCode;
}
