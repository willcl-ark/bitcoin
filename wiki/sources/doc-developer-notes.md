---
kind: source
title: doc/developer-notes.md
status: active
last_reviewed: 2026-04-21
paths:
  - doc/developer-notes.md
tags:
  - developer-notes
  - contributor-guidance
  - tooling
  - style
---

# doc/developer-notes.md

## Source metadata

- Source type: local documentation / contributor guide.
- Primary path: `doc/developer-notes.md`.
- Scope: coding style, debugging and analysis tooling, locking/thread guidance,
  RPC and internal interface conventions, and contributor maintenance
  practices.
- Best use in the wiki: contributor workflow and design-intent context. For
  current runtime behavior, the code and tests remain the primary sources.

## Summary

`doc/developer-notes.md` is a broad maintainer-facing guide for authoring and
reviewing Bitcoin Core changes. It starts with style rules around
`src/.clang-format`, naming, comments, and function signatures, then collects
practical local workflows for `clang-tidy`, Doxygen docs, coverage,
sanitizers, `perf`, valgrind, and IWYU. Later sections capture repository-wide
conventions that matter when maintaining wiki pages about logging, locking,
thread structure, GUI boundaries, source organization, RPC evolution,
`src/interfaces/` abstraction boundaries, subtree updates, LevelDB upgrades,
scripted diffs, and release notes.

For wiki maintenance, this document is most useful as a map of contributor
expectations and supported tooling, not as the final authority on live
codepaths. Any claim about current behavior still needs re-verification in the
checkout.

## Facts extracted

- The document treats `src/.clang-format` as the C++ formatting baseline,
  prefers the newer style in new patches, and explicitly discourages style-only
  churn (`doc/developer-notes.md`, "Coding Style (General)",
  "Coding Style (C++)"; `src/.clang-format`).
- It records preferred C++ API patterns: input parameters before in-out/output
  parameters, direct returns over output parameters where practical,
  `std::optional` for optional by-value inputs, explicit namespace
  qualification to avoid ADL surprises, and Doxygen-compatible comment forms
  for generated docs (`doc/developer-notes.md`,
  "Coding Style (C++ functions and methods)",
  "Coding Style (Doxygen-compatible comments)").
- It points Python style questions to `test/functional/README.md`, so that
  README is the local follow-on source for Python contributor conventions
  (`doc/developer-notes.md`, "Coding Style (Python)";
  `test/functional/README.md`).
- It documents supported local analysis and documentation workflows:
  `clang-tidy`, the `docs` build target, coverage generation,
  `include-what-you-use`, `perf`, and sanitizer builds/suppressions
  (`doc/developer-notes.md`, "Running clang-tidy",
  "Generating Documentation", "Compiling for test coverage",
  "Using IWYU", "Performance profiling with perf", "Sanitizers";
  `test/sanitizer_suppressions/`).
- For debugging, it calls out `RelWithDebInfo` as the default CMake build
  type, recommends `Debug` when source correspondence matters, and uses
  `debug.log`, `-debug`, `-loglevel`, and the `logging` RPC as core
  diagnostics (`doc/developer-notes.md`, "Compiling for debugging",
  "debug.log").
- The notes distinguish internal-check helpers by failure mode:
  `assert`/`Assert` for unsafe-to-continue bugs, `CHECK_NONFATAL` for
  recoverable internal logic bugs, and `Assume` for explicit nonfatal
  assumptions (`doc/developer-notes.md`, "Assertions and Checks";
  `src/util/check.h`).
- Locking guidance emphasizes consistent lock ordering, `-DDEBUG_LOCKORDER`,
  thread-safety annotations on declarations, runtime lock assertions in
  definitions, scope-sensitive `LOCK`/`TRY_LOCK` usage, and a preference for
  `Mutex` over `RecursiveMutex` (`doc/developer-notes.md`,
  "Locking/mutex usage notes", "Threads and synchronization").
- The thread inventory section is a useful orientation aid because it names
  major service threads and their roles, but it is still documentation and
  should be rechecked in code before using it as proof of current thread
  ownership (`doc/developer-notes.md`, "Threads").
- Logging guidance is normative about severity and operator cost: `LogDebug`
  for category-gated diagnostics, sparse unconditional `LogInfo`,
  `LogWarning` for admin-actionable issues, `LogError` for shutdown-level
  failures, and `LogTrace` for very noisy category-gated logs
  (`doc/developer-notes.md`, "Logging").
- Source organization guidance favors self-contained includes, implementations
  in `.cpp` rather than `.h` when possible, explicit namespaces, and scripts in
  Python or Rust rather than bash except where the repository has a specific
  bash-based scripted-diff process (`doc/developer-notes.md`, "Scripts",
  "Source code organization", "Scripted diffs";
  `test/lint/commit-script-check.sh`).
- GUI guidance draws a hard boundary between model and view responsibilities
  and warns against adding potentially blocking `interfaces::Node` or
  `interfaces::Wallet` calls on the GUI thread
  (`doc/developer-notes.md`, "GUI"; `src/interfaces/node.h`,
  `src/interfaces/wallet.h`).
- RPC guidance is extensive and maintenance-relevant: naming and JSON parsing
  conventions, `AmountFromValue`/`ValueFromAmount` for monetary values,
  treating missing and `null` arguments the same, maintaining
  `vRPCConvertParams`, keeping wallet and non-wallet methods separate,
  preferring object responses, using `-deprecatedrpc=` for incompatible
  changes, and documenting integer `verbosity` semantics for new RPCs
  (`doc/developer-notes.md`, "RPC interface guidelines";
  `src/rpc/client.cpp`, `src/rpc/util.h`).
- Internal interface guidance treats `src/interfaces/` as the abstraction
  boundary between node, wallet, GUI, and mining-facing components, favors
  pure-virtual abstract classes with serializable types, avoids overloaded
  methods, and uses lowerCamelCase method names inside interface classes
  (`doc/developer-notes.md`, "Internal interface guidelines",
  "Internal interface naming style"; `src/interfaces/chain.h`,
  `src/interfaces/node.h`, `src/interfaces/wallet.h`,
  `src/interfaces/mining.h`).
- Subtree, LevelDB, release-note, and scripted-diff sections are process
  guidance for repository maintenance rather than runtime behavior. They are
  particularly relevant when future wiki work covers vendored libraries,
  consensus-risk in storage changes, or release engineering
  (`doc/developer-notes.md`, "Subtrees", "Upgrading LevelDB",
  "Scripted diffs", "Release notes").

## Wiki pages updated

- `[[sources/doc-developer-notes]]` - Source summary for the maintainer guide
  in `doc/developer-notes.md`.
- No other wiki pages were edited in this task's scope.

## Open questions

- Which existing wiki pages should cite this document explicitly for
  process/tooling guidance: `[[areas/build-packaging-and-ci]]`,
  `[[areas/testing]]`, `[[areas/rpc-rest-zmq-and-interfaces]]`, or a future
  contributor-workflow page?
- Should the wiki add a dedicated page for debugging and analysis workflows
  (`clang-tidy`, sanitizers, coverage, `perf`, valgrind, IWYU), or is the
  current area coverage sufficient?
- The document's thread list is useful orientation, but should a future
  investigation re-verify each listed thread against current startup code
  before the wiki depends on it for concurrency claims?
