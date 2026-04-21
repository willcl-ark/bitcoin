---
kind: area
title: Mempool and Policy
status: active
last_reviewed: 2026-04-21
paths:
  - src/validation.cpp
  - src/txmempool.h
  - src/txmempool.cpp
  - src/policy/
  - src/rpc/mempool.cpp
tags:
  - mempool
  - policy
  - packages
  - rbf
---

# Mempool and Policy

## Summary

The mempool stores transactions that are valid for the current best chain and are candidates for the next block, but mempool admission is stricter than consensus. Current policy is split between `src/validation.cpp` (`MemPoolAccept`), `src/txmempool.*` (`CTxMemPool` and `TxGraph` integration), and `src/policy/*` for fee, package, RBF, and TRUC rules.

## Responsibilities and invariants

- `CTxMemPool` owns the node-local unconfirmed set, transaction indexes, the outpoint-to-spender map (`mapNextTx`), rolling fee floor, and a sequence number for external tracking. See `src/txmempool.h` (`CTxMemPool`) and `src/txmempool.cpp` (`GetMinFee`, `TrimToSize`, `removeForBlock`).
- Cluster and mining-order logic live below the mempool interface in `TxGraph`. The mempool comment explicitly says this layer partitions transactions into connected clusters, enforces cluster size limits, produces block-inclusion ordering, and selects removals after reorgs. See `src/txmempool.h` (`CTxMemPool`) and `doc/policy/mempool-design.md`.
- Policy does not define consensus. `MemPoolAccept::PreChecks()` and `ConsensusScriptChecks()` reuse consensus routines like `CheckTransaction()` and `Consensus::CheckTxInputs()`, but standardness, fee floors, RBF, package topology, cluster limits, and TRUC are local relay/mining policy. Block connection re-validates consensus in `src/validation.cpp` (`Chainstate::ConnectBlock`).
- Default cluster policy is 64 transactions and 101 kvB per cluster. Package limits are kept within those cluster limits (`MAX_PACKAGE_COUNT`, `MAX_PACKAGE_WEIGHT`). See `src/policy/policy.h` and `src/policy/packages.h`.
- Reorg reinsertion is policy-sensitive. `Chainstate::MaybeUpdateMempoolForReorg()` re-adds disconnected transactions with `bypass_limits=true`, then updates descendants and removes entries that are no longer final or mature. Its comment explicitly notes that TRUC rules are not re-applied there, so reorgs can temporarily create TRUC-policy violations. See `src/validation.cpp` (`MaybeUpdateMempoolForReorg`).

## Important code paths

### Single-transaction admission

- Local single-tx submission goes through `src/validation.cpp` (`ChainstateManager::ProcessTransaction`) and then `AcceptToMemoryPool()`.
- `MemPoolAccept::AcceptSingleTransactionInternal()` applies:
  - `PreChecks()` for structure, coinbase exclusion, standardness, finality, duplicate/conflict checks, input availability, BIP68-at-tip, fee accounting, fee floor, and TRUC prechecks.
  - `ReplacementChecks()` when there are conflicts.
  - cluster-size checks via `CTxMemPool::ChangeSet::CheckMemPoolPolicyLimits()`.
  - `PolicyScriptChecks()` before `ConsensusScriptChecks()` to avoid expensive work on obviously failing transactions.
  - `FinalizeSubpackage()` plus `LimitMempoolSize()` for real submissions.

### Package admission

- `src/validation.cpp` (`ProcessNewPackage`) is the package entry point.
- Test-only package validation uses `MemPoolAccept::AcceptMultipleTransactionsInternal()` through `ATMPArgs::PackageTestAccept`.
- Real package submission uses `MemPoolAccept::AcceptPackage()` through `ATMPArgs::PackageChildWithParents`, and only supports a single transaction or a child-with-parents topology. It de-duplicates already-in-mempool transactions, retries reconsiderable cases as a package, applies package feerate rules, package RBF rules, cluster checks, and then trims the mempool if needed.
- Package RBF is narrower than generic single-tx replacement: `MemPoolAccept::PackageRBFChecks()` requires a 1-parent-1-child package with no in-mempool ancestors for the new transactions. See `src/validation.cpp` and `src/policy/rbf.h`.

### Replacement, fees, and eviction

- Single-tx RBF uses `GetEntriesForConflicts()`, `PaysForRBF()`, and `ImprovesFeerateDiagram()`. The current policy checks replacement effects on affected clusters, not just direct conflicts. See `src/policy/rbf.h`, `src/policy/rbf.cpp`, and `src/txmempool.cpp` (`ChangeSet::CalculateChunksForRBF`).
- `CTxMemPool::TrimToSize()` evicts the worst chunk, bumps the rolling minimum fee by the removed feerate plus incremental relay fee, and uncaches now-unused coins. `GetMinFee()` decays that rolling floor over time. See `src/txmempool.cpp` (`TrimToSize`, `GetMinFee`).
- `CTxMemPool::removeForBlock()` removes confirmed transactions and conflicts after a block connects. `Chainstate::MaybeUpdateMempoolForReorg()` and `CTxMemPool::UpdateTransactionsFromBlock()` rebuild correct in-mempool dependency edges after reorg resurrection.

### Inspection surfaces

- `src/rpc/mempool.cpp` exposes the local mempool view, including cluster inspection (`getmempoolcluster`), feerate diagrams (`getmempoolfeeratediagram`), single/package test admission (`testmempoolaccept`), and real package submission (`submitpackage`).

## Related tests

- `src/test/mempool_tests.cpp` (`MempoolRemoveTest`, ancestry tests, size-limit tests)
- `src/test/txpackage_tests.cpp` (`package_validation_tests`, `package_submission_tests`, `package_cpfp_tests`, `package_rbf_tests`)
- `test/functional/mempool_accept.py`
- `test/functional/mempool_packages.py`
- `test/functional/mempool_package_limits.py`
- `test/functional/mempool_package_rbf.py`
- `test/functional/mempool_truc.py`
- `test/functional/mempool_ephemeral_dust.py`

## Adjacent pages

- `[[workflows/transaction-acceptance]]`
- `[[workflows/block-validation-and-connection]]`
- `[[areas/validation-and-chainstate]]`
- `[[concepts/fee-estimation]]`
- `[[concepts/package-policy-and-relay]]`
- `[[concepts/resource-exhaustion-and-backpressure]]`
- `[[concepts/transaction-sender-and-receiver-privacy]]`
- `[[files/src/txmempool.cpp]]`
- `[[investigations/critical-codepaths-priority-map]]`

## Sources consulted

- `src/validation.cpp` (`MemPoolAccept`, `AcceptToMemoryPool`, `ProcessNewPackage`, `MaybeUpdateMempoolForReorg`)
- `src/txmempool.h` (`CTxMemPool`, `ChangeSet`)
- `src/txmempool.cpp` (`UpdateTransactionsFromBlock`, `removeForBlock`, `TrimToSize`, `GetMinFee`, `ChangeSet::CalculateChunksForRBF`)
- `src/policy/policy.h`
- `src/policy/packages.h`
- `src/policy/rbf.h`
- `src/policy/truc_policy.h`
- `src/rpc/mempool.cpp`
- `doc/policy/mempool-design.md`
