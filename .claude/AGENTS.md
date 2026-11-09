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
  - Modern C++20 best practices

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

- C++20
- RAII
- Zero exceptions
- Explicit error handling using Result<T>
- constexpr whenever possible
- noexcept whenever possible
- [[nodiscard]] where appropriate
- Small focused functions
- Clear naming
- No duplicated logic

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
