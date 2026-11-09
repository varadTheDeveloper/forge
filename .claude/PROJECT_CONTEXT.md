# Forge Core

## Vision

Forge Core is a modern C++20 systems library being built as the foundation of
the Forge JavaScript Runtime.

The goal is to build a production-quality runtime that can eventually
compete with Node.js, Deno, and Bun.

Forge Core is the low-level platform library that provides memory,
containers, filesystem, threading, synchronization, networking and platform
abstractions. The runtime itself (`forge/forge.cpp`, `forge/main.cpp`) embeds
SpiderMonkey and is expected to be rebuilt on top of Forge Core over time,
rather than using raw `std::` containers and the event-loop code currently
prototyped there.

Everything must be production quality.

---

# Philosophy

- Zero exceptions
- Explicit error handling
- Cross-platform
- RAII
- UTF-8 internally
- Small, clean API
- No unnecessary dependencies
- Performance first
- Readability over cleverness

---

# Development Rules

Every component follows this workflow.

1. Design API
2. Review API
3. Freeze API
4. Implement in small parts
5. Review implementation
6. Freeze implementation

Once frozen, APIs are NOT redesigned except for:

- Bugs
- Security issues
- Incorrect behaviour
- Fundamental design flaws

Never redesign frozen components. See `HISTORY.md` for what is already
frozen before touching any existing header. See `AGENTS.md` for how an AI
agent specifically should approach a task in this repo (review checklist,
"be honest about compiling", the include/naming pitfalls already hit
once).

---

# Error Handling

Constructors never fail.

Functions that may fail return `Result<T>` (or `ResultVoid` for functions
with no value to return).

Objects requiring allocation during creation use a static factory:

```
Create(...)
```

Example: `String::Create(...)` instead of `String(...)`.

`Error` is a small value type (`ErrorCode` + native platform code), storing
no message, no stack trace, no formatting. It never allocates and never
throws. See `forge/forge-core/Error.md` for the full frozen spec and
`forge/forge-core/Error.h` for the implementation.

`Failure` exists purely to disambiguate constructing a failed `Result<T>`
from a `Result<T>` that legitimately holds an `Error` as its value type.

---

# Memory

All allocations go through `memory::Allocator`.

Never use:

- `new`
- `delete`
- `malloc`
- `free`

Allocator contract:

- `Allocate(size, alignment)` returns `nullptr` on failure.
- `Deallocate(memory, size, alignment)` must receive the same size/alignment
  used during the matching `Allocate()` call.

`DefaultAllocator` is the current concrete allocator. `UniquePtr`/`MakeUnique`
and `Vector` are built on top of the `Allocator` interface, not on
`std::allocator`.

---

# Current Progress

Completed (API + implementation present in `forge/forge-core/`):

- Types (`Types.h` — fixed-width integer/float aliases, `Size`, `Offset`, `Byte`)
- Error (`Error.h`, spec in `Error.md`)
- Failure (`Failure.h`)
- Result / ResultVoid (`Result.h`, `Result.inl`, `ResultStorage.h`,
  `ResultVoid.h`, `ResultVoid.inl`, `ResultFwd.h`)
- Assert (`Assert.h`)
- Allocator / DefaultAllocator (`memory/Allocator.h`,
  `memory/DefaultAllocator.h/.cpp`, `memory/detail/AllocationBackend.h/.cpp`)
- UniquePtr / MakeUnique (`memory/UniquePtr.h/.inl`, `memory/MakeUnique.h/.inl`)
- Vector (`memory/Vector.h/.inl`)

All of the above compiled cleanly and passed a runtime test (including
allocator-failure paths) as of 2026-07-26 — see `HISTORY.md` for the list
of bugs that pass fixed. Before that date this had never actually been
compiled end-to-end.

`forge/platform/` (Windows path handling groundwork) existed earlier in
the project but has been deliberately removed; the filesystem layer
(Phase 3) will be designed from scratch rather than building on it. See
`HISTORY.md`.

Not started:

- String / StringView / Span
- Array / HashMap / HashSet / Queue / Stack
- Filesystem
- Threading
- Networking
- Runtime integration (replacing `std::vector`/`std::unique_ptr` and the
  ad-hoc `TimerQueue`/`ForgeJobQueue` in `forge.cpp` with Forge Core types)

See `ROADMAP.md` for the intended order of the remaining work, and
`HISTORY.md` for decisions already frozen on the completed components.

---

# Coding Style

- Header-only implementations use `.inl`
- Public API in `.h`
- Inline function bodies in `.inl`
- `constexpr` whenever possible
- `noexcept` whenever possible
- `[[nodiscard]]` where appropriate
- Namespace: `forge::core` for core library types
- Includes: relative, directory-correct paths (`"Types.h"`, `"../Error.h"`,
  `"memory/ResultFwd.h"`) — never a `forge/core/...`-style rooted path.
  The physical directory is `forge/forge-core/` (hyphenated), and relative
  includes work regardless of how the surrounding build's include roots
  are configured. See `HISTORY.md` for why this matters (most of the
  codebase used the wrong convention until the 2026-07-26 pass).
- If a member function's name matches a type visible in the enclosing
  namespace (e.g. `Error()` returning `Error&`, `Size()` returning `Size`),
  GCC hard-errors (`-Wchanges-meaning`) rather than just warning. Fix with
  the `class Error` elaborated-type-specifier for real class types, or by
  fully qualifying the alias target (`forge::core::Size`) when the type is
  a `using`-alias. Do not rename the accessor to work around this — see
  `AGENTS.md` for the full explanation.

Component specs (like `Error.md`) follow this shape: Purpose, Responsibilities,
Non-Goals, Design Goals, Public API, Memory Layout, Ownership, Error Handling
Policy, Thread Safety, Dependencies, Extensibility, Acceptance Criteria,
Implementation Status. Write a matching spec doc before freezing any new
component's API.

---

# Long-Term Goal

Forge Core is not intended to be merely another container library.

It is the complete systems foundation for the Forge JavaScript Runtime.

Every design decision should consider:

- Performance
- Maintainability
- Explicit APIs
- Consistency
- Future runtime requirements
