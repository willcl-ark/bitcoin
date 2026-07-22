# C++

This guide captures coding conventions for Bitcoin Core C++ code, supplementing [doc/developer-notes.md](../doc/developer-notes.md) and [.clang-format](../.clang-format).

## Style & Formatting

- Follow the project's `.clang-format` rules; run `clang-format` on all new/modified code before committing.
- Use `snake_case` for variables and arguments, `m_` for class members, and
  PascalCase for new classes, functions, and methods.
- Wrap multi-line `if`/`else`/loop bodies in braces `{}`; add braces when touching existing code that lacks them.
- Avoid side effects inside `assert()` or `Assume()` — assign results to variables first.

## Casts & Types

- Use C++ named casts (`static_cast`, `reinterpret_cast`, etc.) instead of C-style casts.
- Prefer `std::array` over raw C arrays; prefer `std::span` over pointer+length parameter pairs.
- Use `std::optional` instead of magic sentinel values (`-1`, `max()`, etc.) for absent values.
- Use `using` declarations over `typedef` for type aliases.
- Use fixed-width integer types (`int32_t`, `uint64_t`, etc.) from `<cstdint>`.

## Headers & Includes

- Include the header corresponding to the `.cpp` file first (self-include) so missing dependencies are caught.
- Include what you use — never rely on transitive includes from other headers.
- Include every header directly used. Keep standard-library and project
  includes separated and follow the surrounding project's include style.

## Classes & Constructors

- Mark single-argument constructors `explicit` to prevent accidental implicit conversions.
- Use RAII for resource management; prefer class-scoped locks and runtime lock assertions (`AssertLockHeld`, `DEBUG_LOCKORDER`).

## Assertions & Error Handling

- Use `Assert()` for checks that must always run; use `Assume()` for checks needed only in debug/fuzz builds.
- Use `Assert(*ptr)` after null checks, not bare dereferences, to document non-null contracts.
- Omit `default` in `switch` over `enum class` so the compiler warns on missing cases.
- Use `CHECK_NONFATAL()` for validation errors in user-facing input. Assertions are for internal invariants only.

## Thread Safety

- Place thread-safety annotations (`EXCLUSIVE_LOCKS_REQUIRED`, `GUARDED_BY`, etc.) in header files, not `.cpp` files.
- Prefer references over pointers for non-null function parameters.

## Containers & Logging

- Use `emplace_back` instead of `push_back` when constructing elements in place.
- Use `LogInfo` (and related structured log macros) instead of the deprecated `LogPrintf`.

Refer to [doc/developer-notes.md](../doc/developer-notes.md) and [.clang-format](../.clang-format) for the full canonical style guide.
