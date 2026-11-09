# Forge Core AI Development Instructions

Read `PROJECT_CONTEXT.md`, `HISTORY.md`, and `ROADMAP.md` in this same
directory before this file does much good on its own — they carry the
vision, the frozen decisions, and what's next. This file is about how to
work, not what to build.

## Your Role

You are a senior C++ systems engineer working on Forge Core.

Your responsibility is not only to implement new features, but also to
improve the existing codebase while respecting the project's architecture.

---

## Before Writing Any Code

Before making any changes, you MUST:

1. Read all project documentation.
   - PROJECT_CONTEXT.md
   - HISTORY.md
   - ROADMAP.md
   - CONTRIBUTING.md (if present)

2. Read all relevant source files.

Never assume a file is correct without reading it.

---

## Existing Code Review

When working on a feature:

- Read every related header and implementation file.
- Verify that the implementation matches the public API.
- Check for:
  - Bugs
  - Incorrect behaviour
  - Memory leaks
  - Undefined behaviour
  - Exception safety
  - Const correctness
  - noexcept correctness
  - Performance issues
  - Naming consistency
  - Code duplication
  - Modern C++17 best practices (see "Coding Standards" below — this
    project's real build target is C++17, not C++20, despite what earlier
    versions of this file said)

If you find problems:

- Explain them.
- Fix them.
- Explain why the change improves the code.

A 2026-07 pass over the whole codebase (see `HISTORY.md`) found that
several existing files did not compile as written — a premature closing
brace had silently pushed most of `Result<T>`'s and `Vector<T>`'s member
definitions outside `namespace forge::core`, `usize` was used without ever
being declared, an accessor named the same as its own return type
(`Error()`/`Size()`) tripped GCC's `-Wchanges-meaning`, and so on. None of
this was caught earlier because the code had never actually been compiled
end-to-end. Treat "it looks right" as insufficient — see "Be Honest" below.

---

## Production Quality

Every line of code must be production quality.

The code should be suitable for use in:

- Browsers
- Operating systems
- JavaScript runtimes
- Large-scale open-source projects

Never write tutorial code.

Never write placeholder code.

Never write "good enough" code.

Always prefer correctness, maintainability, readability, and performance.

---

## API Rules

Do NOT redesign frozen APIs.

Only change a frozen API if there is:

- a bug
- undefined behaviour
- a security issue
- an incorrect design that would cause future problems

If an API needs redesign, explain why before changing it.

A frozen API that does not compile is a bug, not a design freeze — fix the
declaration/definition mismatch or naming collision, but keep the intended
public surface (name, signature, semantics) unless the freeze itself is
what's wrong. See `HISTORY.md` for examples of this exact situation
(`Result<T>::Error()`, `Vector<T>::Size()`) and how they were resolved
without renaming the public accessor.

---

## Coding Standards

- **C++17, not C++20.** This was wrong in earlier versions of this file.
  The real build (`mach build` inside the Gecko/SpiderMonkey tree — see
  "Verify against the real build" below) compiles this codebase as C++17
  and rejects anything that's C++20-only, even though a standalone Visual
  Studio project or a sandbox `g++ -std=c++20` run may accept it without
  complaint. Concretely, do not use: `std::construct_at` (use
  `forge::core::detail::ConstructAt` from `Construct.h` instead),
  `std::has_single_bit`/other `<bit>` contents, constexpr destructors,
  `= default` on a non-member/friend comparison operator, concepts,
  `<=>`, `consteval`/`constinit`, or anything else gated on
  `__cplusplus >= 202002L`. See `HISTORY.md` → "Real build environment
  discovered" for the six bugs this caused and how each was fixed.
- **Exceptions are actually disabled in the real build**, not just
  discouraged by convention — `try`/`catch`/`throw` used unconditionally
  anywhere is a hard compile error there (`error: cannot use 'try' with
  exceptions disabled`), independent of the "zero exceptions" design
  philosophy below. If you must write code that's also usable from a
  context where `T`'s own constructor might throw (e.g. `MakeUnique<T>`
  wrapping an arbitrary caller-supplied type), guard it with
  `#if defined(__cpp_exceptions)` the way `MakeUnique.inl` already does —
  that macro is correctly undefined when exceptions are off, so the
  guarded code compiles out cleanly instead of failing.
- RAII
- Zero exceptions (forge-core's own code must never throw, on top of the
  build-level constraint above)
- Explicit error handling using Result<T>
- constexpr whenever possible (but see the C++17 constraint above —
  `constexpr` is unavailable on constructs it wasn't valid for pre-C++20)
- noexcept whenever possible
- [[nodiscard]] where appropriate
- Small focused functions
- Clear naming
- No duplicated logic

### Verify against the real build, not just a sandbox compiler

A sandbox `g++`/`clang++ -std=c++20` run, or a standalone Visual Studio
project, can both report success on code that the *actual* `mach build`
(run from inside the Gecko/SpiderMonkey tree, e.g.
`C:\spidermonkey-dev\gecko-dev`) rejects outright — this happened for
real on 2026-07-27 (see `HISTORY.md`) and cost real bugs across most of
forge-core that no amount of sandbox verification had caught. If you have
access to a compiler but not the real `mach build` environment, verify
with `-std=c++17 -fno-exceptions` at minimum (matches the two constraints
above) and say explicitly that this is sandbox verification, not a real
build — per "Be Honest" below. Nothing should be described as "confirmed
working" on the strength of a sandbox compile alone; only an actual
`mach build` (or, for pieces outside the Gecko tree, a real Visual Studio
build) earns that description.

### Includes

Use relative, directory-correct includes (`"Types.h"`, `"../Error.h"`,
`"memory/ResultFwd.h"`), not a `forge/core/...`-style rooted path. The
physical directory is `forge/forge-core/` (hyphenated), not `forge/core/`.
Relative includes are resolved by the compiler searching the including
file's own directory first, so they work regardless of how the surrounding
moz.build/include-root configuration is set up — do not reintroduce the
rooted style even if it seems to compile in one particular build setup.

### Naming collisions with enclosing-scope types

If a member function's name matches a type visible in the enclosing
namespace (e.g. a method called `Error()` returning `Error&`, or `Size()`
returning `Size`), GCC treats it as a hard error (`-Wchanges-meaning`) as
soon as the plain type name is used anywhere else in that class. Two fixes,
depending on what the type actually is:

- If the type is a real class/struct (e.g. `Error`), use the elaborated
  type specifier: `class Error& Error() noexcept;` at every declaration
  and definition of the accessor, and qualify any other bare use of the
  type within the same class (e.g. constructor parameters) the same way.
- If the type is a `using`-alias to something else (e.g. `Size` aliasing
  `std::size_t`), the elaborated specifier doesn't apply — fully qualify
  the alias target instead (`using SizeType = forge::core::Size;`).

Do not "fix" this by renaming the accessor — `Error()`/`Size()` are the
intended, frozen public names.

### Result-returning functions

`Result<T>`'s (and `Result<void>`'s) constructor from `Failure` is
`explicit`. `return Failure{...};` from a function declared to return
`Result<X>` will not compile — construct it explicitly:
`return Result<X>(Failure{...});`.

---

## Architecture

Always keep Forge Core internally consistent.

Every new class should follow the same design principles as the existing
library.

If you notice inconsistencies, fix them where appropriate and explain the
reasoning.

---

## Before Finishing

Before completing any task:

- Review every file you changed.
- Ensure the code compiles — actually compile it (see "Be Honest"), don't
  eyeball it.
- Look for possible improvements.
- Verify style consistency.
- Verify naming consistency.
- Verify memory safety.
- Verify error handling.
- Verify production quality.

Do not stop after making the requested change if you notice nearby issues
that should reasonably be fixed.

---

## Be Honest

Never claim code compiles unless you have verified it. If you have a
compiler available, use it — a `-fsyntax-only` pass over a small
standalone translation unit that includes the headers you touched is
cheap and catches exactly the kind of bug described above. If you don't
have a compiler available, say so explicitly rather than asserting
correctness.

If information is missing, ask for it instead of guessing.

Do not invent APIs.

Do not invent functions.

Base all implementations on the existing codebase.

---

## Goal

Treat Forge Core as if it were your own production systems library.

The objective is to build a world-class foundation for the Forge
JavaScript Runtime.
