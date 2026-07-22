# Testing

Behavior-changing commits must include corresponding test changes in the same commit. Each commit must build and pass tests independently. Prefer regtest for local testing; use signet/testnet4 for internet or multi-machine tests.

## General Principles

- Add unit, functional, fuzz, or regression coverage where appropriate.
- Prefer deterministic, observable behavior over sleep/log matching or timing-based synchronization.
- Keep tests resource-conscious. Document manual verification when automation is insufficient.
- Do not place test-only code (helpers, mocks, debugging functions) in production source files, headers, or RPC interfaces.
- Avoid tests that only verify standard library or platform behavior.

## C++ Unit Tests

- Use `BasicTestingSetup` (or common utilities) instead of custom setup code in benchmarks.
- Prefer typed Boost assertions (`BOOST_REQUIRE_EQUAL`, `BOOST_CHECK_EQUAL`, `BOOST_CHECK_LE`) for better diagnostics.
- Use `BOOST_REQUIRE` (or `BOOST_REQUIRE_EQUAL`) over `BOOST_CHECK` when a failure invalidates subsequent checks.
- Avoid duplicating setup code; place shared fixture classes in common test headers.

## Functional Tests (Python)

- Use test framework assertion helpers (`assert_equal`, `assert_greater_than`) rather than bare `assert`.
- Use `self.generate()` rather than direct mining RPC calls; specify which nodes are expected synced.
- Use `mocktime` (`setmocktime` + `bumpmocktime`) instead of wall-clock `sleep()` or timeouts.
- For P2P synchronization, prefer `sync_send_with_ping`, `wait_for_disconnect`, `wait_until` with `getpeerinfo`, or other explicit methods over `assert_debug_log` or timing-based sleep.
- Avoid relying on debug log output (`assert_debug_log`) as the primary means of asserting behavioral correctness — prefer RPCs and observable interfaces.
- Tests must be self-contained, not depend on state from previous subtests or test ordering.
- Avoid mutable default argument values; use `None` and generate the value inside the function body.
- Follow Python conventions: f-strings over concatenation, `pathlib` over `os.path`, keyword-only arguments for optional parameters.
- Prefer RPC/integration tests over unit tests for code paths reachable from real user operations; use unit tests for well-contained internal logic.

## Fuzz Targets

- Fuzz harnesses must be deterministic based only on the fuzz input. Avoid mutable global state (global wallets, SPKMs, salted hashers without reseeding) that persists across iterations.
- Use `LIMITED_WHILE` with `ConsumeBool()` for bounded loops rather than manual remaining-bytes checks or bare `while` loops.
- Avoid calling `FuzzedDataProvider` methods directly in function call arguments due to unspecified evaluation order.

## References

- [Bitcoin Core test documentation](../test/README.md)
- [Functional test framework](../test/functional/test_framework/README.md)
- [Fuzzing documentation](../doc/fuzzing.md)
