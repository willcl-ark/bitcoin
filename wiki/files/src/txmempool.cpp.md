---
kind: file
title: src/txmempool.cpp
status: active
last_reviewed: 2026-04-21
paths:
  - src/txmempool.cpp
tags:
  - mempool
  - resources
  - policy
---

# src/txmempool.cpp

## Role in the System

`src/txmempool.cpp` implements the main storage and resource-management behavior
of the mempool. It maintains transaction graph state, descendant/ancestor
relationships, rolling minimum fee behavior, expiry, trimming, block-connection
removal, and dynamic memory accounting.

## Important Types and Functions

- `CTxMemPool::removeForBlock()` removes confirmed transactions and conflicts.
- `CTxMemPool::UpdateTransactionsFromBlock()` rebuilds dependency edges after
  disconnected transactions are re-added.
- `CTxMemPool::Expire()` removes old entries and dependent descendants.
- `CTxMemPool::GetMinFee()` exposes the rolling floor derived from prior
  trimming pressure.
- `CTxMemPool::TrimToSize()` is the main size-limiting and eviction path.
- `CTxMemPool::DynamicMemoryUsage()` reports current tracked memory usage.
- `TxGraph` ownership and cluster behavior are surfaced through `CTxMemPool`
  and declared in `src/txmempool.h`.

## Callers and Dependencies

- Called by mempool acceptance, reorg repair, mining, and fee-estimator update
  paths.
- Depends on transaction graph state (`TxGraph`), mempool options, and
  validation/mempool synchronization rules declared in `src/txmempool.h`.
- Interacts with mining and fee estimation because trimming and block removal
  directly change the set of available transactions and the rolling minimum fee
  floor.

## Related Tests

- `src/test/mempool_tests.cpp`
- `src/test/txpackage_tests.cpp`
- `src/test/policyestimator_tests.cpp`
- `src/test/fuzz/tx_pool.cpp`
- `test/functional/mempool_limit.py`
- `test/functional/interface_usdt_mempool.py`

## Notes or Risks

- Critical categories:
  - `resource`: this file is a primary OOM/CPU/memory-pressure boundary for the
    node.
  - `offline`: severe trimming or inconsistent update/removal behavior can
    destabilize relay/mining behavior and degrade normal node operation.
  - `sender/receiver privacy`: eviction and floor-fee behavior indirectly
    change relay visibility and transaction propagation patterns.
- `TrimToSize()` and `GetMinFee()` are especially important when reasoning
  about bounded memory use under adversarial transaction load.
- `UpdateTransactionsFromBlock()` and `removeForBlock()` are reorg-sensitive:
  mistakes here can leave the mempool inconsistent after block connection or
  disconnection.

## Sources Consulted

- `src/txmempool.h`
- `src/txmempool.cpp`
- `src/test/mempool_tests.cpp`
- `src/test/txpackage_tests.cpp`
- `src/test/policyestimator_tests.cpp`
- `src/test/fuzz/tx_pool.cpp`
- `test/functional/mempool_limit.py`
- `test/functional/interface_usdt_mempool.py`
