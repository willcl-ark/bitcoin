---
kind: overview
title: Bitcoin Core Codebase Overview
status: active
last_reviewed: 2026-04-21
paths:
  - README.md
  - CONTRIBUTING.md
  - doc/
  - src/
  - test/
tags:
  - overview
  - architecture
---

# Bitcoin Core Codebase Overview

## Summary

Bitcoin Core is the reference full node implementation used to validate blocks
and transactions, relay them on the Bitcoin peer-to-peer network, expose
operator and application interfaces, and optionally provide wallet and GUI
functionality. In this worktree, the current checkout is the primary source of
truth for behavior. High-level process and subsystem boundaries are described
in `README.md`, `CONTRIBUTING.md`, `doc/developer-notes.md`,
`src/node/README.md`, `src/interfaces/README.md`, `src/test/README.md`, and
`test/README.md`.

## Major Areas

- [[areas/consensus-and-script]] covers block, transaction, and script rules
  implemented under `src/consensus/`, `src/script/`, and `src/primitives/`.
- [[areas/validation-and-chainstate]] covers block acceptance, chainstate
  ownership, and activation paths centered on `src/validation.cpp`,
  `src/validation.h`, `src/kernel/`, and `src/node/`.
- [[areas/mempool-and-policy]] covers non-consensus transaction admission and
  relay policy under `src/policy/`, `src/txmempool.*`, package acceptance, and
  fee/policy-related paths.
- [[areas/p2p-and-networking]] covers transport, peer management, relay, and
  download logic under `src/net*`, `src/net_processing*`, `src/addrman*`,
  `src/banman*`, `src/txrequest*`, and `src/node/txdownloadman*`.
- [[areas/mining-and-block-assembly]] covers candidate block construction,
  mining RPCs, and the internal mining interface centered on
  `src/node/miner.*` and `src/rpc/mining.cpp`.
- [[areas/wallet]] covers optional wallet code under `src/wallet/`, wallet RPC,
  and wallet-specific tests.
- [[areas/rpc-rest-zmq-and-interfaces]] covers the RPC server, REST handlers,
  ZMQ notifications, and internal interfaces under `src/rpc/`, `src/rest.*`,
  `src/zmq/`, and `src/interfaces/`.
- [[areas/common-utils-and-configuration]] covers configuration parsing,
  reusable helpers, logging, and utility code under `src/common/`,
  `src/util/`, `src/logging/`, and related support code.
- [[areas/gui]] covers the Qt application, node/wallet models, wallet views,
  and GUI-specific startup/tests under `src/qt/`.
- [[areas/testing]] covers unit, functional, fuzz, GUI, benchmark, and lint
  infrastructure across `src/test/`, `src/wallet/test/`, `src/qt/test/`,
  `src/test/fuzz/`, `test/functional/`, `test/fuzz/`, and `test/lint/`.
- [[areas/build-packaging-and-ci]] covers the CMake build, depends system,
  packaging helpers, and CI definitions rooted in `CMakeLists.txt`, `cmake/`,
  `depends/`, `ci/`, `contrib/`, and `share/`.
- [[areas/libbitcoinkernel-and-libraries]] covers the experimental
  `bitcoinkernel` library and vendored or tightly integrated libraries such as
  `src/secp256k1/`, `src/univalue/`, `src/leveldb/`, `src/minisketch/`, and
  `src/crc32c/`.

## Architectural Boundaries

`src/node/README.md` describes `src/node/` as code that needs access to shared
node state such as chain, block index, UTXO, and mempool state. It also states
that `src/node/`, `src/wallet/`, and `src/qt/` should avoid directly calling
one another, using `src/interfaces/` as the boundary instead.

`src/interfaces/README.md` lists the internal C++ interfaces used to separate
node, wallet, GUI, IPC, and RPC-facing components. That separation matters for
ownership and review because many end-to-end workflows cross subsystem
boundaries without collapsing them into one module.

Optional subsystems are controlled by build flags in `CMakeLists.txt`. Current
examples include `ENABLE_WALLET`, `BUILD_GUI`, `WITH_ZMQ`, `ENABLE_IPC`,
`BUILD_TESTS`, `BUILD_BENCH`, and `BUILD_FUZZ_BINARY`. Wiki pages should call
out these gates whenever they affect whether a behavior exists in a build.

## End-to-End Workflows

The current workflow pages focus on the cross-subsystem paths that most often
matter for review or orientation:

- [[workflows/transaction-acceptance]] for mempool admission and policy checks.
- [[workflows/block-validation-and-connection]] for block processing,
  validation, and active chain updates.
- [[workflows/initial-block-download]] for IBD state, headers-first sync, and
  related gating behavior.
- [[workflows/rpc-request-handling]] for HTTP JSON-RPC ingress, routing, and
  wallet-aware dispatch.
- [[workflows/block-relay]] for headers, compact-block, full-block, and
  outbound announcement behavior.
- [[workflows/node-startup-and-shutdown]] for `bitcoind` lifecycle, warmup,
  interrupt propagation, and ordered teardown.
- [[workflows/wallet-rescan]] for wallet load/import/restore rescans, block
  filter acceleration, and prune/assumeutxo boundaries.

These workflows cut across consensus, validation, policy, networking, RPC,
wallet, and testing, so they are a useful bridge between subsystem pages and
file-level investigations.

## Cross-Cutting Concepts

The wiki also has concept pages for reusable ideas that span more than one
subsystem:

- [[concepts/chainstate]] and [[concepts/assumeutxo]] for UTXO-state ownership
  and snapshot lifecycle.
- [[concepts/descriptors]] and [[concepts/script-verification]] for script and
  wallet-facing spend-description concepts.
- [[concepts/addrman]] and [[concepts/fee-estimation]] for networking and
  policy subsystems with dedicated internal data models.
- [[concepts/package-policy-and-relay]] for package shape rules, real
  child-with-parents submission, package RBF/TRUC policy, and the current 1p1c
  relay path.
- [[concepts/operator-privacy]] for operator identity and metadata leak
  boundaries across networking, RPC, and wallet-facing surfaces.
- [[concepts/rpc-authentication-and-wallet-routing]] for the boundary between
  HTTP reachability/auth, RPC method authorization, and later
  `/wallet/<walletname>` selection.
- [[concepts/transaction-sender-and-receiver-privacy]],
  [[concepts/resource-exhaustion-and-backpressure]], and
  [[concepts/wallet-fund-safety]] for the cross-cutting invariants most likely
  to matter in high-impact reviews.

## Source Summaries

The `wiki/sources/` layer now captures the local documents that establish
architecture and contributor context before drilling into runtime behavior:

- [[sources/doc-developer-notes]] for contributor tooling, style, logging,
  locking, RPC, and interface conventions.
- [[sources/src-node-readme]] and [[sources/src-interfaces-readme]] for the
  intended boundary between node, wallet, GUI, and interface code.
- [[sources/src-test-readme]] and [[sources/test-readme]] for the compiled and
  end-to-end test layers that back many behavior claims elsewhere in the wiki.

## Critical-Focused Starting Points

The highest-priority review and documentation targets are now grouped in
[[investigations/critical-codepaths-priority-map]]. It maps the current tree's
most important crash, offline, resource, fund-safety, and privacy-sensitive
paths into concrete files and tests. The critical file pages now cover:

- [[files/src/addrman.cpp]]
- [[files/src/init.cpp]]
- [[files/src/net.cpp]]
- [[files/src/net_processing.cpp]]
- [[files/src/node/txorphanage.cpp]]
- [[files/src/txmempool.cpp]]
- [[files/src/wallet/load.cpp]]
- [[files/src/wallet/spend.cpp]]
- [[files/src/wallet/wallet.cpp]]
- [[files/src/httprpc.cpp]]
- [[files/src/node/miner.cpp]]
- [[files/src/node/txdownloadman_impl.cpp]]
- [[files/src/validation.cpp]]

Availability-sensitive review also now has dedicated workflow pages for
[[workflows/node-startup-and-shutdown]], [[workflows/block-relay]], and
[[workflows/wallet-rescan]].

Coverage prioritization now also has
[[investigations/critical-test-coverage-gaps]], which points at critical paths
whose current unit, fuzz, or functional coverage looks thinner than their
impact.

## Testing and Build Context

`src/test/README.md` defines the Boost-based unit test structure around
`test_bitcoin` and points to `src/test/` and `src/wallet/test/` as the primary
unit-test roots. `test/README.md` describes functional, fuzz, and lint test
layers, and the top-level `README.md` points contributors toward `ctest` and
`build/test/functional/test_runner.py` as the standard entry points. These
testing layers are part of the architecture: many subsystem claims are only
useful if they are tied back to the tests that exercise them.

## How To Use This Wiki

Start from [[index]] for navigation. For architecture questions, read the most
relevant area page first, then a workflow page if the question crosses
subsystems, then a concept page if the question is about a reusable internal
model, and finally re-check the cited code paths in the current tree before
relying on the wiki for a high-risk conclusion.

## Sources Consulted

- `README.md`
- `CONTRIBUTING.md`
- `doc/README.md`
- `doc/developer-notes.md`
- `src/node/README.md`
- `src/interfaces/README.md`
- `src/test/README.md`
- `test/README.md`
- `CMakeLists.txt`
