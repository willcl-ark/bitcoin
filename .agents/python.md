# Python

This guide covers Python conventions for Bitcoin Core, complementing the canonical
[test/functional/README.md](../test/functional/README.md) style rules.

## Formatting & Style

- Follow **PEP 8** (two blank lines between top-level definitions, one between constants and functions; no parentheses around `if`/`while` conditions).
- Use **snake_case** for variables, functions, and parameters; `ALL_CAPS` for true constants only.
- Prefer **f-strings** over `%`-formatting, `.format()`, or string concatenation.
- Prefix private methods/members with a single underscore `_`; never use `__` (name mangling) outside special methods.

## Imports

- Sort imports **alphabetically** within each section. Add a **trailing comma** after the last import.
- Use **one import per line** for functional-test multi-line imports (see `test/functional/README.md`).
- No wildcard (`*`) imports.

## Paths & Serialization

- Use **pathlib.Path** instead of `os.path`: `/` operator, `.resolve()`, `.parent`, `.is_file()`, etc.
- Prefer **`int.to_bytes()` / `int.from_bytes()`** over `struct.pack` / `struct.unpack` for single-value serialization.

## Functions & Defaults

- Avoid **mutable default arguments**: use `None` and construct inside the body.
- Use **keyword-only arguments** (`*` in signature) for boolean flags, integral literals, timeout/duration values, and similar option-like parameters.
- Pass **named (keyword) arguments** at call sites when supplying boolean or integral literals.

## Functional Test Framework

- Use the framework's **assertion helpers** (`assert_equal`, `assert_greater_than`, etc.) rather than bare `assert` for comparisons.
- Pass `assert_debug_log` a **list of strings** (`['msg']`), not a bare string.
- Use **`self.generate(...)`** rather than a bare `generate(...)` call.
- Do not duplicate default argument values from framework methods; retrieve them via the argument interface (e.g., `self.Arg<bool>(2)`).

## Dict Access

- Use **`dict[key]`** when a missing key should fail; use **`dict.get(key, default)`** for optional lookups with a fallback.

## Testing-Only Cryptography

- Add a **warning header** to test-only crypto modules noting they are for testing only, slow, and not side-channel resistant.
