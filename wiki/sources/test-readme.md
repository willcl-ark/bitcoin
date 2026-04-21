---
kind: source
title: test/ README
status: active
last_reviewed: 2026-04-21
source_path: test/README.md
tags:
  - source
  - testing
  - functional-tests
  - fuzz
  - lint
---

# test/ README

## Source metadata

- Source type: local documentation
- Path: `test/README.md`
- Scope: functional, fuzz, and lint test layers plus local test-running guidance

## Summary

`test/README.md` documents the integration-test layer outside `src/`: fuzz
runner entry points, functional-test workflow, lint scripts, and the main
local debugging guidance for `test_runner.py`. It is the best high-level local
source for how end-to-end and static-analysis coverage is expected to be run.

## Facts extracted

- The directory is split into fuzz, functional, and lint layers, with
  functional tests exercising `bitcoind` and `bitcoin-qt` through RPC and P2P
  interfaces.
- The standard functional harness is
  `build/test/functional/test_runner.py`, which defaults to four parallel jobs
  and supports `--extended` for a larger run set.
- Backward-compatibility tests require first fetching prior releases with
  `test/get_previous_releases.py`.
- The document calls out extra dependencies for some test families, including
  ZMQ and IPC Python libraries.
- It describes practical debugging surfaces: RAM-disk acceleration, cache
  invalidation, process cleanup, and log-file locations after failures.

## Wiki pages updated

- `[[overview]]`
- `[[index]]`

## Open questions

- The README explains how to run test layers, but not which Bitcoin Core
  invariants rely mainly on functional coverage versus unit or fuzz coverage in
  the current tree.
