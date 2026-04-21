---
kind: source
title: src/test/ README
status: active
last_reviewed: 2026-04-21
source_path: src/test/README.md
tags:
  - source
  - testing
  - unit-tests
---

# src/test/ README

## Source metadata

- Source type: local documentation
- Path: `src/test/README.md`
- Scope: Boost-based unit and GUI test structure and contributor workflow

## Summary

`src/test/README.md` describes the Boost-based unit-test runner layout around
`test_bitcoin`, how unit and GUI tests are built and run, and the naming and
placement conventions for new test files. It is the main local source for how
the C++ test layer is expected to be used by contributors.

## Facts extracted

- Unit tests are built into `build/bin/test_bitcoin`, with common setup rooted
  in `src/test/util/setup_common.cpp`.
- `ctest --test-dir build` is the standard aggregated entry point for compiled
  tests, while `build/bin/test_bitcoin` and `build/bin/test_bitcoin-qt` can be
  run directly.
- Test suites should follow the `<source_filename>_tests` naming convention and
  be added to `src/test/CMakeLists.txt` or `src/wallet/test/CMakeLists.txt`.
- The runner accepts both Boost test arguments and selected `bitcoind`
  arguments after `--`, which matters for debugging and logging behavior.
- `-testdatadir` preserves test data directories for debugging instead of
  deleting them after the run.

## Wiki pages updated

- `[[overview]]`
- `[[index]]`

## Open questions

- This document covers the unit-test contributor workflow, but it does not map
  which subsystems are weakly covered; that still has to be derived from the
  current test tree.

