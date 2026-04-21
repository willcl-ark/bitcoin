---
kind: workflow
title: Block Validation and Connection
status: active
last_reviewed: 2026-04-20
paths:
  - src/validation.cpp
  - src/validation.h
  - src/net_processing.cpp
tags:
  - block-validation
  - chainstate
  - reorg
---

# Block Validation and Connection

## Summary

The hot path for full blocks is `ProcessNewBlock() -> AcceptBlock() -> ActivateBestChain()`. Header checks, contextual block checks, disk persistence, UTXO updates, reorg handling, and mempool reconciliation are deliberately split across those stages in `src/validation.cpp`.

## Workflow

### 1. Initial block sanity and storage

1. `src/validation.cpp` (`ChainstateManager::ProcessNewBlock`) locks `::cs_main` and runs `CheckBlock()` before anything is written.
2. The code comment notes that `CheckBlock()` failures are intentionally not cached as block invalidity. This avoids permanently marking a block invalid if an unknown malleation issue causes `CheckBlock()` to fail.
3. If `CheckBlock()` succeeds, `AcceptBlock()`:
   - calls `AcceptBlockHeader()`
   - ignores many unrequested blocks unless they have enough work and are close enough to the active height
   - runs `CheckBlock()` again plus `ContextualCheckBlock()`
   - writes the block to disk and records received transactions through `ReceivedBlockTransactions()`

### 2. Header and contextual boundaries

- `AcceptBlockHeader()` applies `CheckBlockHeader()` plus `ContextualCheckBlockHeader()` and only stores new headers when the caller proved `min_pow_checked=true`.
- `AcceptBlock()` applies `ContextualCheckBlock()` before block data becomes eligible for activation.
- If the new block extends the active tip and the node is not in IBD, `AcceptBlock()` can emit `NewPoWValidBlock` before the chain tip is actually activated.

### 3. Best-chain activation

- `Chainstate::ActivateBestChain()` serializes itself with `m_chainstate_mutex`, repeatedly finds the best candidate with `FindMostWorkChain()`, and advances toward it through `ActivateBestChainStep()`.
- `ActivateBestChainStep()`:
  - disconnects blocks back to the fork point with `DisconnectTip()`
  - connects blocks toward the most-work tip with `ConnectTip()`
  - restores mempool consistency after any reorg via `MaybeUpdateMempoolForReorg()`
- The step logic intentionally works in limited batches and may return after forward progress so `::cs_main` is not held for the entire catch-up or reorg.

### 4. Connecting one tip block

- `ConnectTip()` loads the block if needed, reuses the dedicated `m_connect_block_view`, and calls `Chainstate::ConnectBlock()`.
- On success it:
  - flushes the connect-block view
  - flushes chainstate if needed
  - removes confirmed/conflicting mempool transactions through `CTxMemPool::removeForBlock()`
  - updates `m_chain`, IBD state, and tip notifications

### 5. UTXO-dependent consensus checks

`Chainstate::ConnectBlock()` is the UTXO-dependent consensus boundary. It:

- re-runs `CheckBlock()`
- enforces BIP30 overwrite protection and the BIP34-related exception logic
- derives script flags for this block with `GetBlockScriptFlags()`
- checks each non-coinbase transaction with `Consensus::CheckTxInputs()`
- enforces BIP68 sequence locks in block context
- counts sigops against the consensus block limit
- runs script checks, optionally in parallel through the validation check queue
- checks the coinbase reward against subsidy plus fees
- writes undo data, raises block index validity to `BLOCK_VALID_SCRIPTS`, and updates the view's best block

One subtle boundary is called out in the source: `ConnectBlock()` does not re-run `ContextualCheckBlock()` or `ContextualCheckBlockHeader()`. Any future consensus rule moved into those contextual functions has upgrade implications.

### 6. Reorg and disconnection handling

- `DisconnectTip()` reads the old tip block, applies undo through `DisconnectBlock()`, moves prune locks back if necessary, and can queue disconnected transactions for re-addition.
- `MaybeUpdateMempoolForReorg()` tries to resurrect disconnected transactions with `AcceptToMemoryPool(..., bypass_limits=true)`, removes now-invalid descendants, updates dependency edges with `UpdateTransactionsFromBlock()`, removes entries that are no longer final or mature, and finally re-trims the mempool.

## Consensus vs policy boundary

- Block connection only cares about consensus validity and chainstate correctness.
- Relay/mempool policy such as standardness, RBF, package rules, and TRUC are not block-validity rules and are not enforced in `ConnectBlock()`.

## Related tests

- `src/test/validation_block_tests.cpp` (`processnewblock_signals_ordering`, `mempool_locks_reorg`)
- `src/test/validation_chainstate_tests.cpp` (`connect_tip_does_not_cache_inputs_on_failed_connect`)
- `src/test/validation_chainstatemanager_tests.cpp` (`invalidate_block_and_reconsider_fork`)
- `test/functional/feature_assumeutxo.py`

## Adjacent pages

- `[[areas/validation-and-chainstate]]`
- `[[areas/mempool-and-policy]]`
- `[[workflows/initial-block-download]]`

## Sources consulted

- `src/validation.cpp` (`ChainstateManager::ProcessNewBlock`, `AcceptBlock`, `CheckBlock`, `ContextualCheckBlockHeader`, `ContextualCheckBlock`, `Chainstate::ActivateBestChain`, `ActivateBestChainStep`, `ConnectTip`, `DisconnectTip`, `ConnectBlock`, `MaybeUpdateMempoolForReorg`)
- `src/validation.h` (`Chainstate::ActivateBestChain`, `ConnectBlock`)
- `src/net_processing.cpp` (`PeerManagerImpl::NewPoWValidBlock`)
