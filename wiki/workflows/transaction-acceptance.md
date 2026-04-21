---
kind: workflow
title: Transaction Acceptance
status: active
last_reviewed: 2026-04-20
paths:
  - src/validation.cpp
  - src/net_processing.cpp
  - src/node/txdownloadman_impl.cpp
  - src/rpc/mempool.cpp
tags:
  - mempool
  - packages
  - relay
---

# Transaction Acceptance

## Summary

Single transactions and packages eventually enter the same validation core: `src/validation.cpp` (`AcceptToMemoryPool` and `ProcessNewPackage`) against the active chainstate under `::cs_main`. The main differences are the entry point, whether the submission is test-only, and whether policy can use package feerates or 1-parent-1-child package logic.

## Entry points

- Local single-tx admission uses `src/validation.cpp` (`ChainstateManager::ProcessTransaction`), which calls `AcceptToMemoryPool()`.
- P2P `tx` messages are handled in `src/net_processing.cpp` (`PeerManagerImpl::ProcessMessage`). If the node is still in IBD, the message is ignored early.
- `testmempoolaccept` in `src/rpc/mempool.cpp` uses:
  - `ChainstateManager::ProcessTransaction(..., test_accept=true)` for one transaction.
  - `ProcessNewPackage(..., test_accept=true)` for multi-transaction input.
- `submitpackage` in `src/rpc/mempool.cpp` uses `ProcessNewPackage(..., test_accept=false)` and only submits a single transaction or a child-with-parents package.

## Single-transaction path

1. `AcceptToMemoryPool()` builds `MemPoolAccept::ATMPArgs::SingleAccept` and calls `MemPoolAccept::AcceptSingleTransactionInternal()`.
2. `MemPoolAccept::PreChecks()` applies the cheap gating rules first:
   - consensus structure via `CheckTransaction()`
   - coinbase exclusion
   - standardness via `IsStandardTx()`
   - finality at the current tip via `CheckFinalTxAtTip()`
   - duplicate/conflict detection against the mempool
   - input existence through `CoinsTip()` plus mempool-backed lookup
   - BIP68-at-tip checks through `CalculateLockPointsAtTip()` and `CheckSequenceLocksAtTip()`
   - consensus input accounting via `Consensus::CheckTxInputs()`
   - witness/standard-script rules, fee floor checks, and TRUC prechecks
3. If the transaction conflicts with mempool entries, `ReplacementChecks()` applies RBF policy, including anti-DoS fee rules and feerate-diagram improvement.
4. `CheckMemPoolPolicyLimits()` ensures the staged result does not exceed cluster policy.
5. `PolicyScriptChecks()` runs before `ConsensusScriptChecks()` so expensive script work is deferred until policy checks pass.
6. Real submissions call `FinalizeSubpackage()`, then `LimitMempoolSize()`. A transaction can therefore validate successfully and still be evicted immediately as `mempool full`, which is returned as a reconsiderable fee failure.

## Package path

### Test-only package path

- `ProcessNewPackage(..., test_accept=true)` uses `MemPoolAccept::AcceptMultipleTransactionsAndCleanup()`.
- `AcceptMultipleTransactionsInternal()` accepts any well-formed package within context-free package limits, runs per-tx `PreChecks()`, package TRUC checks, package-feerate checks when requested, optional package RBF checks, cluster checks, and `PolicyScriptChecks()`, but does not submit to the mempool.

### Real package submission path

- `ProcessNewPackage(..., test_accept=false)` uses `MemPoolAccept::AcceptPackage()`.
- `AcceptPackage()` only supports:
  - a single transaction
  - a child-with-parents package whose last element is the child
- It first de-duplicates already-in-mempool transactions, then tries remaining transactions individually. Transactions that only fail for reconsiderable reasons or missing inputs may be retried as a package.
- Package admission can use aggregate package feerate, package RBF, cluster checks, and ephemeral-dust checks before `SubmitPackage()` applies the result and `LimitMempoolSize()` trims if necessary.

## P2P retry and orphan/package fallback

- `src/node/txdownloadman_impl.cpp` (`TxDownloadManagerImpl::ReceivedTx`) may skip re-validating already-known or recently rejected transactions.
- If a transaction is in the reconsiderable-rejects filter, `Find1P1CPackage()` looks for a matching orphan child from the same peer and returns a 1-parent-1-child package candidate.
- `src/net_processing.cpp` (`PeerManagerImpl::ProcessInvalidTx`) records a rejection and may return a package to retry.
- `PeerManagerImpl::ProcessPackageResult()` then turns per-tx package results back into relay-side side effects such as `ProcessValidTx()` or renewed rejection bookkeeping.

## Consensus vs policy boundary

- Consensus checks reused here include `CheckTransaction()`, `Consensus::CheckTxInputs()`, and the current-block script-flag recheck in `ConsensusScriptChecks()`.
- Policy-only checks include standardness, mempool fee floor, package topology, RBF replacement economics, cluster limits, TRUC rules, and ephemeral-dust policy.
- Passing transaction acceptance does not make a transaction consensus-valid forever; block connection rechecks the actual block-context rules later in `src/validation.cpp` (`Chainstate::ConnectBlock`).

## Related tests

- `src/test/txpackage_tests.cpp`
- `src/test/mempool_tests.cpp`
- `test/functional/mempool_accept.py`
- `test/functional/mempool_packages.py`
- `test/functional/mempool_package_limits.py`
- `test/functional/mempool_package_rbf.py`
- `test/functional/mempool_truc.py`
- `test/functional/mempool_ephemeral_dust.py`
- `test/functional/p2p_ibd_txrelay.py`

## Adjacent pages

- `[[areas/mempool-and-policy]]`
- `[[workflows/block-validation-and-connection]]`
- `[[areas/validation-and-chainstate]]`
- `[[concepts/resource-exhaustion-and-backpressure]]`
- `[[concepts/transaction-sender-and-receiver-privacy]]`
- `[[concepts/wallet-fund-safety]]`
- `[[files/src/txmempool.cpp]]`
- `[[files/src/wallet/spend.cpp]]`

## Sources consulted

- `src/validation.cpp` (`ChainstateManager::ProcessTransaction`, `MemPoolAccept::PreChecks`, `ReplacementChecks`, `PolicyScriptChecks`, `ConsensusScriptChecks`, `AcceptSingleTransactionInternal`, `AcceptMultipleTransactionsInternal`, `AcceptPackage`, `AcceptToMemoryPool`, `ProcessNewPackage`)
- `src/net_processing.cpp` (`PeerManagerImpl::ProcessMessage`, `ProcessInvalidTx`, `ProcessPackageResult`)
- `src/node/txdownloadman_impl.cpp` (`ReceivedTx`, `MempoolRejectedTx`, `Find1P1CPackage`)
- `src/rpc/mempool.cpp` (`testmempoolaccept`, `submitpackage`)
