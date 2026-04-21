---
kind: file
title: src/validation.cpp
status: active
last_reviewed: 2026-04-21
paths:
  - src/validation.cpp
tags:
  - validation
  - chainstate
  - mempool
---

# src/validation.cpp

## Role in the System

`src/validation.cpp` is the main integration file for Bitcoin Core block,
transaction, and chainstate validation. It is where context-free block checks,
UTXO-dependent block connection, mempool admission, chain activation, reorg
repair, IBD updates, and assumeutxo snapshot handling are tied together.

## Important Types and Functions

- Script and transaction checking:
  `CheckInputScripts`, `CScriptCheck::operator()`
- Chainstate block connection and activation:
  `Chainstate::ConnectBlock`, `ConnectTip`, `DisconnectTip`,
  `ActivateBestChainStep`, `ActivateBestChain`
- Mempool/reorg repair:
  `Chainstate::MaybeUpdateMempoolForReorg`, `ChainstateManager::ProcessTransaction`
- Header/block acceptance:
  `CheckBlock`, `ContextualCheckBlockHeader`, `ContextualCheckBlock`,
  `ChainstateManager::AcceptBlockHeader`, `AcceptBlock`, `ProcessNewBlock`
- Disk/cache handling:
  `FlushStateToDisk`, `ForceFlushStateToDisk`, `LoadChainTip`, `ReplayBlocks`
- Assumeutxo lifecycle:
  `ActivateSnapshot`, `PopulateAndValidateSnapshot`,
  `LoadAssumeutxoChainstate`, `MaybeRebalanceCaches`, `ActivateBestChains`

## Callers and Dependencies

- RPC, networking, and import paths eventually feed blocks or transactions into
  `ProcessNewBlock()` and `ProcessTransaction()`.
- The file depends heavily on:
  - `src/validation.h` for shared types and declarations
  - consensus checks from `src/consensus/*`
  - the script interpreter and signature cache from `src/script/*`
  - mempool structures from `src/txmempool.*`
  - block/index/disk helpers from node and blockmanager code
- Mining and test code call back into validation through `TestBlockValidity()`
  and chainstate APIs defined alongside this file.

## Related Tests

- `src/test/validation_chainstate_tests.cpp`
- `src/test/validation_chainstatemanager_tests.cpp`
- `src/test/validation_block_tests.cpp`
- `src/test/txvalidationcache_tests.cpp`
- `test/functional/feature_assumeutxo.py`
- `test/functional/mempool_accept.py`

## Notes and Risks

- The file carries several important boundary comments:
  - `ConnectBlock()` does not rerun `ContextualCheckBlock()` or
    `ContextualCheckBlockHeader()`.
  - `ProcessNewBlock()` keeps `CheckBlock()` under `::cs_main` because
    `CBlock::fChecked` is not thread-safe.
  - `ActivateBestChain()` intentionally releases and reacquires locks between
    steps to avoid long critical sections.
- Because this file mixes mempool policy, consensus connection, and snapshot
  lifecycle code, it is a common place where subsystem boundaries have to be
  checked carefully during review.

## Sources Consulted

- `src/validation.cpp`
- `src/validation.h`
