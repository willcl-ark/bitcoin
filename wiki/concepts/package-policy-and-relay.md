---
kind: concept
title: Package Policy and Relay
status: active
last_reviewed: 2026-04-21
paths:
  - src/policy/packages.h
  - src/policy/packages.cpp
  - src/policy/truc_policy.h
  - src/policy/truc_policy.cpp
  - src/policy/ephemeral_policy.cpp
  - src/txmempool.cpp
  - src/validation.cpp
  - src/node/txdownloadman.h
  - src/node/txdownloadman_impl.cpp
  - src/net_processing.cpp
  - src/rpc/mempool.cpp
tags:
  - mempool
  - package-policy
  - relay
  - truc
  - rbf
---

# Package Policy and Relay

## Summary

Current package behavior is split across three layers:

- context-free package shape checks in `src/policy/packages.*`
- package mempool acceptance in `src/validation.cpp`
  (`ProcessNewPackage`, `MemPoolAccept`)
- opportunistic 1-parent-1-child retry/relay in
  `src/node/txdownloadman_impl.cpp` and `src/net_processing.cpp`

The current tree, not older package-relay proposals, is authoritative for
current behavior. `doc/policy/packages.md` is useful for motivation, but the
operative rules are the current symbols below. In this tree:

- multi-transaction `testmempoolaccept` validates any well-formed package up to
  `MAX_PACKAGE_COUNT`, but it does not use package feerates and does not allow
  replacement because `MemPoolAccept::ATMPArgs::PackageTestAccept()` sets
  `m_package_feerates=false` and `m_allow_replacement=false`
- `submitpackage` only submits a single transaction or a child-with-parents
  package, and the RPC frontend further restricts multi-transaction submissions
  to `IsChildWithParentsTree()` in `src/rpc/mempool.cpp`
- P2P "package relay" is not a general package transport in the current tree;
  the network layer only synthesizes a 1p1c retry package through
  `node::PackageToValidate`

## Responsibilities and Invariants

### Package object and topologies

- `src/policy/packages.h` defines `Package` as an ordered
  `std::vector<CTransactionRef>`.
- `src/policy/packages.cpp` (`IsWellFormedPackage`) enforces the context-free
  rules:
  - no more than `MAX_PACKAGE_COUNT = 25` transactions
  - total transaction weight no more than `MAX_PACKAGE_WEIGHT = 404000`
  - no duplicates
  - no conflicting prevout spends inside the package
  - parents must appear before children
- `src/policy/packages.cpp` (`IsChildWithParents`) defines the real-submission
  topology as "last transaction is the child; every earlier transaction must be
  one of that child's direct parents."
- `src/policy/packages.cpp` (`IsChildWithParentsTree`) adds one more
  restriction: the parents may not depend on each other. `submitpackage` checks
  this at the RPC boundary before calling `ProcessNewPackage()`.
- `src/node/txdownloadman.h` (`PackageToValidate`) is even narrower than the
  RPC path: it explicitly constructs only a 1-parent-1-child package.

### Real submission vs test-only validation

- `src/validation.cpp` (`ProcessNewPackage`) has two materially different
  modes.
- Test-only mode calls `MemPoolAccept::AcceptMultipleTransactionsAndCleanup()`
  with `ATMPArgs::PackageTestAccept()`. This path can evaluate general
  well-formed packages, but it does not submit to the mempool, does not allow
  replacement, and does not use aggregate package feerates.
- Real submission mode calls `MemPoolAccept::AcceptPackage()` with
  `ATMPArgs::PackageChildWithParents()`. This path is for single transactions
  and child-with-parents packages only, enables package feerates, and may use
  package RBF.
- `MemPoolAccept::AcceptPackage()` first tries each transaction on its own.
  Transactions already accepted or already present in the mempool are excluded
  from later package feerate accounting. Only transactions that failed for
  `TX_RECONSIDERABLE` or `TX_MISSING_INPUTS` are retried as a package.
- This means real package submission is not atomic. `validation.h`
  (`ProcessNewPackage`) documents that partial submission is possible, and
  `src/test/txpackage_tests.cpp` (`package_submission_tests`,
  `package_single_tx`) exercises cases where a parent remains accepted while a
  child still fails.

### Package mempool checks

- Per-transaction policy still runs first. `src/validation.cpp`
  (`MemPoolAccept::PreChecks`) does structure checks, standardness, finality,
  missing-input lookup, BIP68-at-tip, `Consensus::CheckTxInputs`, witness
  standardness, sigops limits, per-transaction fee-floor checks when package
  feerates are disabled, `PreCheckEphemeralTx()`, and `SingleTRUCChecks()`.
- After all package transactions pass `PreChecks()`,
  `MemPoolAccept::AcceptMultipleTransactionsInternal()` applies the
  package-only checks:
  - `PackageTRUCChecks()` in `src/policy/truc_policy.cpp`
  - aggregate package feerate checks when `m_package_feerates=true`
  - `PackageRBFChecks()` when the package conflicts with mempool entries
  - `CTxMemPool::ChangeSet::CheckMemPoolPolicyLimits()`
  - `CheckEphemeralSpends()` in `src/policy/ephemeral_policy.cpp`
  - `PolicyScriptChecks()`
  - `SubmitPackage()` for real submissions
- `SubmitPackage()` returns success before the final mempool trim.
  `AcceptPackage()` then calls `LimitMempoolSize()` and rewrites results to
  `"mempool full"` if a newly accepted transaction was immediately evicted.
- Same-txid-different-witness entries are deduplicated rather than replaced.
  `validation.h` (`MempoolAcceptResult::ResultType::DIFFERENT_WITNESS`) and
  `src/test/txpackage_tests.cpp` (`package_witness_swap_tests`) cover this
  behavior.

### Package policy limits

- `src/policy/packages.h` keeps the context-free package limits within default
  cluster policy via `static_assert(DEFAULT_CLUSTER_LIMIT >= MAX_PACKAGE_COUNT)`
  and
  `static_assert(MAX_PACKAGE_WEIGHT <= DEFAULT_CLUSTER_SIZE_LIMIT_KVB * WITNESS_SCALE_FACTOR * 1000)`.
- Runtime package admission also enforces cluster limits, not just package
  limits. `src/txmempool.cpp`
  (`CTxMemPool::ChangeSet::CheckMemPoolPolicyLimits`) rejects changesets whose
  `TxGraph::Level::TOP` view is oversized.
- The default runtime cluster policy comes from `src/policy/policy.h` and
  `src/kernel/mempool_limits.h`:
  - `DEFAULT_CLUSTER_LIMIT = 64`
  - `DEFAULT_CLUSTER_SIZE_LIMIT_KVB = 101`
- `src/init.cpp` labels `-limitancestorcount` and `-limitdescendantcount` as
  deprecated mempool-admission settings, replaced by `-limitclustercount` and
  `-limitclustersize`. For package acceptance in the current tree, the active
  hard mempool limits are cluster-based.
- Package feerate uses modified fees, so `prioritisetransaction` deltas can
  affect package admission. `src/test/txpackage_tests.cpp`
  (`package_cpfp_tests`) covers both negative and positive deltas.

### Package RBF

- `src/validation.cpp` (`MemPoolAccept::PackageRBFChecks`) only supports a
  1p1c replacement package. If the candidate replacement is not size 2 or does
  not satisfy `IsChildWithParents()`, it fails with
  `"package RBF failed: package must be 1-parent-1-child"`.
- The new package may not have in-mempool ancestors. The source comment notes
  that relaxing this would require revisiting how package temporary coins and
  ancestor/removal intersections are tracked.
- Package RBF reuses the regular replacement machinery:
  `GetEntriesForConflicts()`, `PaysForRBF()`, and
  `ImprovesFeerateDiagram()`.
- It also adds a package-specific shape rule: package feerate must be strictly
  greater than parent feerate, so the child is actually paying to improve the
  chunk.
- `test/functional/mempool_package_rbf.py` and
  `src/test/txpackage_tests.cpp` (`package_rbf_tests`) cover the supported
  replacement shape, anti-DoS fee checks, conflict-cluster cap, and failure
  cases.

### 1p1c, TRUC, and relay behavior

- `src/policy/truc_policy.h` treats `version=3` as TRUC and hardcodes the
  package-relevant limits:
  - `TRUC_ANCESTOR_LIMIT = 2`
  - `TRUC_DESCENDANT_LIMIT = 2`
  - `TRUC_MAX_VSIZE = 10000`
  - `TRUC_CHILD_MAX_VSIZE = 1000`
- `SingleTRUCChecks()` can return a sibling eligible for eviction in a single
  transaction context, but package contexts disable that path:
  `ATMPArgs::PackageTestAccept()` and `ATMPArgs::PackageChildWithParents()`
  both set `m_allow_sibling_eviction=false`.
- `PackageTRUCChecks()` enforces the package-side invariants that matter for
  1p1c/TRUC policy:
  - direct parent version inheritance between TRUC and non-TRUC transactions
  - no TRUC child that would exceed ancestor count 2
  - no TRUC parent that would end up with more than one descendant
  - no TRUC child above 1000 vB when it has an unconfirmed parent
  - no package pattern that creates a TRUC grandparent-parent-child chain
- `test/functional/mempool_truc.py` exercises the current package behavior for
  TRUC ancestor limits, package inheritance, package/testmempoolaccept
  differences, and the fact that package submission does not use sibling
  eviction in the same way as single-transaction admission.
- `src/node/txdownloadman_impl.cpp` is the current relay-side package logic.
  `ReceivedTx()` and `MempoolRejectedTx()` may return a
  `PackageToValidate` when a transaction is in
  `RecentRejectsReconsiderableFilter()` and a matching orphan child exists.
- `Find1P1CPackage()` only considers orphan children from the same peer and
  only returns a package if the child itself is not in
  `RecentRejectsFilter()` and the package hash from `GetPackageHash()` is not
  already in the reconsiderable-rejects filter.
- `src/net_processing.cpp` (`PeerManagerImpl::ProcessPackageResult`) assumes
  the current relay-side package is exactly size 2, caches package-wide reject
  state via `MempoolRejectedPackage()` on failure, and then translates per-tx
  package results back into normal `ProcessValidTx()` or `ProcessInvalidTx()`
  side effects.
- `test/functional/p2p_1p1c_network.py` covers the current network story: a
  submitted 1p1c package can propagate through nodes that previously saw the
  parent alone, the child alone as an orphan, or the parent as a low-fee
  reject. That test does not exercise a distinct package P2P message; it
  exercises the current tx-download/orphan retry path.

## Important Code Paths

- Context-free package rules:
  `src/policy/packages.h`, `src/policy/packages.cpp`
  (`IsWellFormedPackage`, `IsChildWithParents`, `IsChildWithParentsTree`,
  `GetPackageHash`)
- Package mempool validation and submission:
  `src/validation.cpp`
  (`MemPoolAccept::PreChecks`, `PackageRBFChecks`,
  `AcceptMultipleTransactionsInternal`, `AcceptPackage`, `SubmitPackage`,
  `ProcessNewPackage`)
- TRUC and ephemeral-dust package policy:
  `src/policy/truc_policy.h`, `src/policy/truc_policy.cpp`
  (`SingleTRUCChecks`, `PackageTRUCChecks`);
  `src/policy/ephemeral_policy.h`, `src/policy/ephemeral_policy.cpp`
  (`PreCheckEphemeralTx`, `CheckEphemeralSpends`)
- Runtime cluster enforcement:
  `src/txmempool.cpp` (`CTxMemPool::ChangeSet::CheckMemPoolPolicyLimits`)
- RPC surfaces:
  `src/rpc/mempool.cpp` (`testmempoolaccept`, `submitpackage`)
- Relay-side 1p1c retry:
  `src/node/txdownloadman.h`, `src/node/txdownloadman_impl.cpp`
  (`PackageToValidate`, `ReceivedTx`, `MempoolRejectedTx`,
  `Find1P1CPackage`, `MempoolRejectedPackage`);
  `src/net_processing.cpp`
  (`PeerManagerImpl::ProcessInvalidTx`, `ProcessPackageResult`)

## Related Tests

- `src/test/txpackage_tests.cpp`
  (`package_sanitization_tests`, `package_submission_tests`,
  `package_witness_swap_tests`, `package_cpfp_tests`, `package_rbf_tests`)
- `src/test/txdownload_tests.cpp`
  (`tx_rejection_types`, `handle_missing_inputs`)
- `test/functional/rpc_packages.py`
  (`test_chain`, `test_multiple_parents`, `test_submitpackage`,
  `test_submitpackage_with_ancestors`)
- `test/functional/mempool_package_limits.py`
- `test/functional/mempool_package_rbf.py`
- `test/functional/mempool_truc.py`
- `test/functional/mempool_ephemeral_dust.py`
- `test/functional/p2p_1p1c_network.py`
- `src/test/fuzz/package_eval.cpp`
  (`ephemeral_package_eval`, `tx_package_eval`)
- `src/test/fuzz/rbf.cpp` (`package_rbf`)
- `src/test/fuzz/txdownloadman.cpp`
  (`txdownloadman`, `txdownloadman_impl`)

## Adjacent Pages

- `[[areas/mempool-and-policy]]`
- `[[areas/p2p-and-networking]]`
- `[[workflows/transaction-acceptance]]`
- `[[concepts/resource-exhaustion-and-backpressure]]`
- `[[files/src/net_processing.cpp]]`
- `[[files/src/validation.cpp]]`

## Open Questions

- `src/validation.cpp` (`MemPoolAccept::PackageRBFChecks`) has an explicit note
  that generalizing package RBF beyond the current no-in-mempool-ancestors
  restriction would require revisiting package temporary-coin handling and the
  missing ancestor/removal intersection checks.
- `src/node/txdownloadman_impl.cpp`
  (`MaybeAddOrphanResolutionCandidate`) still has a TODO to add
  orphan-resolution-specific limits and delays instead of only mirroring normal
  tx-request tracking limits.

## Sources Consulted

- `src/policy/packages.h`
- `src/policy/packages.cpp`
- `src/policy/policy.h`
- `src/policy/truc_policy.h`
- `src/policy/truc_policy.cpp`
- `src/policy/rbf.h`
- `src/policy/ephemeral_policy.h`
- `src/policy/ephemeral_policy.cpp`
- `src/kernel/mempool_limits.h`
- `src/txmempool.cpp`
- `src/validation.h`
- `src/validation.cpp`
- `src/node/txdownloadman.h`
- `src/node/txdownloadman_impl.cpp`
- `src/net_processing.cpp`
- `src/rpc/mempool.cpp`
- `src/init.cpp`
- `doc/policy/packages.md`
- `src/test/txpackage_tests.cpp`
- `src/test/txdownload_tests.cpp`
- `src/test/fuzz/package_eval.cpp`
- `src/test/fuzz/rbf.cpp`
- `src/test/fuzz/txdownloadman.cpp`
- `test/functional/rpc_packages.py`
- `test/functional/mempool_package_limits.py`
- `test/functional/mempool_package_rbf.py`
- `test/functional/mempool_truc.py`
- `test/functional/mempool_ephemeral_dust.py`
- `test/functional/p2p_1p1c_network.py`
