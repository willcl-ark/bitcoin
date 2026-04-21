---
kind: area
title: Common Utils and Configuration
status: active
last_reviewed: 2026-04-21
paths:
  - src/common/
  - src/util/
tags:
  - configuration
  - args
  - settings
  - util
---

# Common Utils and Configuration

## Summary

`src/common/` owns process-wide configuration parsing, settings persistence, and cross-platform startup helpers. `src/util/` provides the reusable low-level building blocks used across the node, wallet, RPC, and test code: filesystem wrappers, string and encoding helpers, time and thread utilities, result types, process helpers, and similar support code.

## Responsibilities and Invariants

- `ArgsManager` in `src/common/args.cpp` is the main settings front end. It parses command-line arguments, exposes typed getters, tracks network-only options, and resolves datadir, config, and settings paths.
- Setting precedence is centralized in `common::MergeSettings()` / `common::GetSetting()` in `src/common/settings.cpp`: forced settings override command line, which overrides read-write `settings.json`, which overrides network-section config values, which override default-section config values.
- `InterpretKey()` in `src/common/args.cpp` handles config-section prefixes and `nofoo` negation. `ParseParameters()` rejects unknown command-line options and disallows non-negated `-includeconf` on the command line.
- `ArgsManager::ReadConfigFiles()` in `src/common/config.cpp` resolves `-conf`, reads `bitcoin.conf`, processes `includeconf` entries from config files, and keeps track of config sections before chain selection is finalized.
- `common::InitConfig()` in `src/common/init.cpp` is the higher-level configuration bootstrap. It validates `-datadir`, reads config files, selects chain params, creates base and network datadirs when missing, creates `wallets/` subdirectories for new datadirs, detects ignored `bitcoin.conf` situations, and verifies that `settings.json` can be read and rewritten.
- `ArgsManager::ReadSettingsFile()` and `WriteSettingsFile()` in `src/common/args.cpp` persist dynamic settings to `<datadir>/<network>/settings.json` via `common::ReadSettings()` and `common::WriteSettings()` in `src/common/settings.cpp`.
- `src/common/system.h` exposes cross-platform helpers such as `SetupEnvironment()`, `SetupNetworking()`, `GetNumCores()`, and `GetTotalRAM()`. `runCommand()` is only declared when `HAVE_SYSTEM`, and `ShellEscape()` is omitted on Windows.
- The `src/util/` subtree is intentionally broad. Representative building blocks include filesystem/path helpers (`util/fs.h`, `util/fs_helpers.h`), strings and encodings (`util/string.h`, `util/strencodings.h`), time and thread helpers (`util/time.h`, `util/thread.h`, `util/threadinterrupt.h`, `util/threadnames.h`), result and assertion helpers (`util/result.h`, `util/expected.h`, `util/check.h`), and process/socket helpers (`util/exec.h`, `util/subprocess.h`, `util/sock.h`).
- `src/util/CMakeLists.txt` packages much of this support code into the `bitcoin_util` static library together with shared support sources such as logging, randomness, streams, and synchronization primitives.

## Important Code Paths

- Argument parsing and path resolution: `src/common/args.cpp`, `src/common/args.h`
- Config-file parsing and include handling: `src/common/config.cpp`
- Settings precedence and JSON serialization: `src/common/settings.cpp`, `src/common/settings.h`
- Startup configuration bootstrap: `src/common/init.cpp`
- Cross-platform process helpers: `src/common/system.h`
- Representative utility clusters: `src/util/fs.cpp`, `src/util/string.cpp`, `src/util/strencodings.cpp`, `src/util/time.cpp`, `src/util/thread.cpp`, `src/util/threadnames.cpp`, `src/util/check.cpp`, `src/util/exec.cpp`

## Compile and Runtime Gates

- Runtime config and persistence knobs: `-conf`, `-noconf`, `-includeconf`/`-noincludeconf`, `-datadir`, `-settings`, `-nosettings`, and network sections handled through `ArgsManager` and `common::InitConfig`
- Runtime ignored-config escape hatch: `-allowignoredconf` in `src/common/init.cpp`
- Platform-specific parsing behavior: Windows command lines normalize `/foo` to `-foo` in `ArgsManager::ParseParameters()` (`src/common/args.cpp`)
- Compile/platform gates in system helpers: `HAVE_SYSTEM` and `WIN32` in `src/common/system.h`

## Related Tests

- Configuration and args: `src/test/argsman_tests.cpp`, `src/test/getarg_tests.cpp`, `src/test/settings_tests.cpp`
- Filesystem and system helpers: `src/test/fs_tests.cpp`, `src/test/system_tests.cpp`
- General utility coverage: `src/test/util_tests.cpp`, `src/test/util_string_tests.cpp`, `src/test/util_check_tests.cpp`, `src/test/util_expected_tests.cpp`, `src/test/util_threadnames_tests.cpp`, `src/test/threadpool_tests.cpp`

## Adjacent Pages

- `[[areas/rpc-rest-zmq-and-interfaces]]`
- `[[areas/wallet]]`
- `[[areas/build-packaging-and-ci]]`
- `[[workflows/node-startup-and-shutdown]]`

## Sources Consulted

- `src/common/args.cpp`
- `src/common/config.cpp`
- `src/common/settings.cpp`
- `src/common/settings.h`
- `src/common/init.cpp`
- `src/common/system.h`
- `src/util/CMakeLists.txt`
