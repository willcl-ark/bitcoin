---
kind: area
title: Build Packaging and CI
status: active
last_reviewed: 2026-04-20
paths:
  - CMakeLists.txt
  - src/CMakeLists.txt
  - depends/
  - ci/
  - contrib/
  - share/
tags:
  - build
  - packaging
  - ci
  - depends
---

# Build Packaging and CI

## Summary

The current tree uses CMake as the primary top-level build system, `depends/`
as the pinned dependency and cross-compilation layer, `ci/` as the scripted CI
orchestration layer, and a mix of `share/` and `contrib/` for deploy-time
assets and helper tooling.

Packaging is intentionally split:

- install rules for in-tree targets live in CMake (`src/CMakeLists.txt`, `src/kernel/CMakeLists.txt`);
- platform assets and helper scripts live under `share/` and `contrib/`;
- broader packaging work is partly out of tree, as `contrib/README.md` points to the separate `bitcoin-core/packaging` repository.

# Responsibilities and invariants

- `CMakeLists.txt` is the top-level switchboard. It defines options for binaries, wallet support, IPC, tests, fuzzing, benches, and the experimental `libbitcoinkernel` target, then hands off to `test/`, `doc/`, and `src/`.
- `src/CMakeLists.txt` assembles the concrete binaries and libraries, includes the vendored-library CMake wrappers, and conditionally adds subdirectories for GUI, tests, benches, fuzzing, and `src/kernel/`.
- `depends/README.md` documents the reproducible dependency builder. The output is not picked up automatically: callers must pass the generated toolchain file, such as `depends/x86_64-pc-linux-gnu/toolchain.cmake`, to the top-level CMake configure step.
- `depends/README.md` also makes the boundary between dependency toggles and feature toggles explicit. For example, `make NO_WALLET=1` in `depends/` is expected to translate into `-DENABLE_WALLET=OFF` when configuring the main build.
- `ci/test_run_all.sh` is intentionally thin: it sources `./ci/test/00_setup_env.sh` and then invokes `./ci/test/02_run_container.py`. The configuration-specific behavior lives in the `ci/test/00_setup_env_*.sh` files described by `ci/README.md`.
- `ci/README.md` states that some CI jobs build through `./depends` specifically to match release dependency versions rather than system packages.

## Important code paths

- `CMakeLists.txt`
  Defines build options, enables testing, and prints the configure summary that records which executables and test targets are active.
- `src/CMakeLists.txt`
  Defines the main executable and library graph and installs binaries through `install_binary_component(...)`.
- `src/kernel/CMakeLists.txt`
  Installs the experimental `bitcoinkernel` library, header, and pkg-config file as the `libbitcoinkernel` component.
- `depends/Makefile` and `depends/README.md`
  Provide the cross-build and dependency-cache layer used both locally and in CI/release workflows.
- `ci/test_run_all.sh`, `ci/test/00_setup_env*.sh`, `ci/test/02_run_container.py`, `ci/lint.py`
  Form the main in-tree CI entry points.
- `contrib/guix/README.md`
  Describes the bootstrappable Guix-based build path used for reproducible release builds and attestations.
- `contrib/verify-binaries/README.md`
  Describes the post-build verification path for published release binaries.
- `share/examples/bitcoin.conf`, `share/setup.nsi.in`, `share/qt/Info.plist.in`, `contrib/init/bitcoind.service`
  Show that deploy-time assets and service/installer templates are kept in-tree even though broader packaging is not fully centralized here.

## Related tests

- Build and test matrix definitions: `ci/test/00_setup_env*.sh`.
- CI lint orchestration: `ci/lint.py`, `ci/lint/`.
- Reproducible-build verification: `contrib/guix/`, `contrib/verify-binaries/`.

## Adjacent pages

- `[[areas/testing]]`
- `[[areas/common-utils-and-configuration]]`
- `[[areas/libbitcoinkernel-and-libraries]]`

## Sources consulted

- `CMakeLists.txt`
- `src/CMakeLists.txt`
- `src/kernel/CMakeLists.txt`
- `depends/README.md`
- `ci/README.md`
- `ci/test_run_all.sh`
- `contrib/README.md`
- `contrib/guix/README.md`
- `contrib/verify-binaries/README.md`
- `share/examples/bitcoin.conf`
- `share/setup.nsi.in`
- `share/qt/Info.plist.in`
- `contrib/init/bitcoind.service`
