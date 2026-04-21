---
kind: area
title: Validation and Chainstate
status: active
last_reviewed: 2026-04-20
paths:
  - src/validation.cpp
  - src/validation.h
  - src/chain.h
tags:
  - validation
  - chainstate
  - assumeutxo
---

# Validation and Chainstate

## Summary

`ChainstateManager` and `Chainstate` own header acceptance, block acceptance, UTXO-set updates, best-chain activation, and initial-block-download state. Current behavior lives primarily in `src/validation.cpp` and `src/validation.h`, with the tip recency/work test in `src/chain.h` (`CChain::IsTipRecent`).

## Responsibilities and invariants

- `ChainstateManager` owns shared validation-wide state: the block index, best header, the set of live chainstates, the cached IBD flag, `MinimumChainWork()`, and the compiled `AssumedValidBlock()` value. See `src/validation.h` (`ChainstateManager`) and `src/validation.cpp` (`ChainstateManager::AcceptBlockHeader`, `ChainstateManager::UpdateIBDStatus`).
- `Chainstate` owns one view of the active UTXO set plus one best-chain view. UTXO-dependent consensus checks happen in `src/validation.cpp` (`Chainstate::ConnectBlock`), while best-tip selection and reorg handling happen in `Chainstate::ActivateBestChain`, `ActivateBestChainStep`, `ConnectTip`, and `DisconnectTip`.
- `Chainstate::ActivateBestChain()` is serialized by `m_chainstate_mutex` and may release `::cs_main` between steps after making forward progress. Callers are explicitly warned not to invoke it while holding `::cs_main` or from validation-interface callbacks. See `src/validation.h` (`Chainstate::ActivateBestChain`) and `src/validation.cpp` (`Chainstate::ActivateBestChain`).
- `ChainstateManager::IsInitialBlockDownload()` is a lock-free read of `m_cached_is_ibd`. The flag only changes in one direction: `UpdateIBDStatus()` can latch it from `true` to `false`, and never back to `true`. Exit requires block loading to be finished and `CurrentChainstate().m_chain.IsTipRecent(MinimumChainWork(), max_tip_age)` to pass. See `src/validation.h` (`m_cached_is_ibd`, `UpdateIBDStatus`) and `src/chain.h` (`CChain::IsTipRecent`).
- Snapshot operation can temporarily create two chainstates. `CurrentChainstate()` tracks the most-work network-tip target, `HistoricalChainstate()` continues full validation toward the snapshot base, and `ValidatedChainstate()` selects the fully validated view for indexers. See `src/validation.h` (`CurrentChainstate`, `HistoricalChainstate`, `ValidatedChainstate`) and `doc/design/assumeutxo.md`.

## Important code paths

### Header path

- `src/validation.cpp` (`ChainstateManager::ProcessNewBlockHeaders`) acquires `::cs_main`, feeds headers through `AcceptBlockHeader()`, and then emits header-tip notifications.
- `AcceptBlockHeader()` runs `CheckBlockHeader()`, checks ancestry and invalid-parent state, applies `ContextualCheckBlockHeader()`, and only stores a new header if the caller proved `min_pow_checked=true`. This is the anti-DoS boundary for permanent header storage.

### Block path

- `src/validation.cpp` (`ChainstateManager::ProcessNewBlock`) runs `CheckBlock()` under `::cs_main` because `CBlock::fChecked` is not thread-safe, then calls `AcceptBlock()`, then activates the best chain on the active chainstate and, if present, on the historical chainstate.
- `AcceptBlock()` gates unsolicited blocks by work, height, and request state, then runs `CheckBlock()` and `ContextualCheckBlock()` before writing block data and marking transactions received. It may relay `NewPoWValidBlock` before activation if the block extends the current active tip and the node is not in IBD.

### Best-chain activation

- `src/validation.cpp` (`Chainstate::FindMostWorkChain`) picks the highest-work candidate that is not known-invalid and still has required block data.
- `Chainstate::ActivateBestChainStep()` disconnects to the fork point, connects blocks in batches, and restores mempool consistency after reorgs through `MaybeUpdateMempoolForReorg()`.
- `Chainstate::ActivateBestChain()` loops these steps, emits `BlockConnected`, `UpdatedBlockTip`, and `ActiveTipChange` notifications, and rebalances caches when the active chainstate leaves IBD.

### UTXO connection boundary

- `src/validation.cpp` (`Chainstate::ConnectBlock`) is where header-independent consensus checks that need the UTXO set are enforced: BIP30 overwrite protection, `Consensus::CheckTxInputs`, BIP68 sequence locks, sigops accounting, script verification, coinbase reward limits, undo creation, and best-block update in the coins view.
- `ConnectBlock()` re-runs `CheckBlock()`, but does not re-run `ContextualCheckBlock()` or `ContextualCheckBlockHeader()`. The code comments call this out as an upgrade-sensitive boundary for any future consensus rule added to those contextual checks.

## Related tests

- `src/test/validation_chainstate_tests.cpp` (`connect_tip_does_not_cache_inputs_on_failed_connect`, `chainstate_update_tip`)
- `src/test/validation_chainstatemanager_tests.cpp` (`chainstatemanager_ibd_exit_after_loading_blocks`, `invalidate_block_and_reconsider_fork`, snapshot tests)
- `src/test/validation_block_tests.cpp` (`processnewblock_signals_ordering`, `mempool_locks_reorg`)
- `test/functional/feature_assumeutxo.py`
- `test/functional/feature_maxtipage.py`

## Adjacent pages

- `[[concepts/chainstate]]`
- `[[concepts/assumeutxo]]`
- `[[workflows/block-validation-and-connection]]`
- `[[workflows/initial-block-download]]`
- `[[areas/mempool-and-policy]]`
- `[[files/src/validation.cpp]]`

## Sources consulted

- `src/validation.h` (`Chainstate`, `ChainstateManager`)
- `src/validation.cpp` (`ChainstateManager::ProcessNewBlockHeaders`, `AcceptBlockHeader`, `ProcessNewBlock`, `AcceptBlock`, `Chainstate::ConnectBlock`, `ConnectTip`, `DisconnectTip`, `ActivateBestChain`, `UpdateIBDStatus`)
- `src/chain.h` (`CChain::IsTipRecent`)
- `doc/design/assumeutxo.md`
