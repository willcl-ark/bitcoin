---
kind: area
title: Testing
status: active
last_reviewed: 2026-04-21
paths:
  - src/test/
  - src/wallet/test/
  - test/
tags:
  - testing
  - unit-tests
  - fuzz
  - functional-tests
---

# Testing

## Summary

Bitcoin Core's test surface is split across compiled test binaries under `src/`
and Python-driven suites under `test/`. The main strata in the current tree are:

- Boost-based unit tests in `src/test/`, built into `test_bitcoin` by `src/test/CMakeLists.txt`.
- Wallet unit tests appended to `test_bitcoin` when `ENABLE_WALLET` is on in `src/wallet/test/CMakeLists.txt`.
- GUI unit tests in `src/qt/test/`, exposed as `test_bitcoin-qt` when `BUILD_GUI_TESTS` is enabled according to `src/test/README.md` and `CMakeLists.txt`.
- Experimental kernel tests in `src/test/kernel/`, built as `test_kernel` when `BUILD_KERNEL_LIB` and `BUILD_KERNEL_TEST` are enabled (`src/CMakeLists.txt`, `src/test/kernel/CMakeLists.txt`).
- Fuzz targets from `src/test/fuzz/`, built into the `fuzz` binary when `BUILD_FUZZ_BINARY` is enabled (`CMakeLists.txt`, `src/test/fuzz/CMakeLists.txt`).
- Python integration suites in `test/`: `test/functional/`, `test/fuzz/`, and `test/lint/` (`test/README.md`).

## Responsibilities and invariants

- `CMakeLists.txt` gates the compiled test surface with `BUILD_TESTS`, `BUILD_GUI_TESTS`, `BUILD_KERNEL_TEST`, and `BUILD_FUZZ_BINARY`.
- `src/test/CMakeLists.txt` defines `test_bitcoin`, links it against `test_util`, and derives one `ctest` entry per Boost suite by scanning the sources in `add_boost_test()` and `add_all_test_targets()`. The test registration is source-driven rather than hand-maintained.
- `src/test/util/README.md` describes `src/test/util/` as the shared, mostly stateless test library. Common global setup lives in `setup_common.cpp`, and binaries instantiate `BasicTestingSetup` or derived fixtures.
- `test/README.md` treats `test/functional/` as end-to-end RPC/P2P coverage, `test/lint/` as static checks, and `test/fuzz/` as a runner for the fuzz targets compiled from `src/test/fuzz/`.
- Functional tests cache a pre-mined 200-block chain under `build/test/cache` to avoid regenerating it on every run (`test/README.md`).

## Important code paths

- `CMakeLists.txt` controls whether test targets are generated at all and enables `ctest` when `BUILD_TESTS` is on.
- `src/test/CMakeLists.txt` is the authoritative manifest for `test_bitcoin` sources and linked libraries.
- `src/wallet/test/CMakeLists.txt` extends `test_bitcoin` with wallet-specific cases instead of creating a separate wallet test binary.
- `src/test/kernel/CMakeLists.txt` defines `test_kernel`, which links directly against `bitcoinkernel`.
- `src/test/fuzz/CMakeLists.txt` defines the `fuzz` executable and links it against `test_fuzz`, `bitcoin_common`, `bitcoin_util`, `leveldb`, `univalue`, `secp256k1`, and `minisketch`.
- `test/functional/test_runner.py`, described in `test/README.md`, is the main harness for parallel functional runs. By default it runs up to four jobs.

## Related tests

- Unit and fixture coverage: `src/test/`, `src/test/util/`.
- Wallet unit coverage: `src/wallet/test/`.
- Kernel library smoke coverage: `src/test/kernel/test_kernel.cpp`.
- Fuzz targets: `src/test/fuzz/`.
- End-to-end daemon, RPC, and P2P coverage: `test/functional/`.
- Static repository policy checks: `test/lint/`.

## Adjacent pages

- `[[areas/build-packaging-and-ci]]`
- `[[areas/libbitcoinkernel-and-libraries]]`
- `[[workflows/transaction-acceptance]]`
- `[[investigations/critical-test-coverage-gaps]]`

## Sources consulted

- `CMakeLists.txt`
- `src/CMakeLists.txt`
- `src/test/CMakeLists.txt`
- `src/test/kernel/CMakeLists.txt`
- `src/test/README.md`
- `src/test/util/README.md`
- `src/test/fuzz/CMakeLists.txt`
- `src/wallet/test/CMakeLists.txt`
- `test/README.md`
