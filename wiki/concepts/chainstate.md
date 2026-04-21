---
kind: concept
title: Chainstate
status: active
last_reviewed: 2026-04-21
paths:
  - src/validation.h
  - src/validation.cpp
  - src/node/chainstate.cpp
tags:
  - chainstate
  - utxo
  - activation
---

# Chainstate

## Summary

A `Chainstate` is Bitcoin Core's validated view of the block tree plus one UTXO
set. It owns tip activation, disconnect/connect sequencing, UTXO cache
management, and replay/load logic for that view. `ChainstateManager` can manage
more than one `Chainstate` at a time, but each `Chainstate` still represents a
single internally consistent coins view.

## Responsibilities and Invariants

- `src/validation.h` (`Chainstate`) owns the coins caches, best-chain view,
  flush/replay logic, and activation machinery for one chainstate.
- UTXO-dependent consensus checks happen inside
  `src/validation.cpp` (`Chainstate::ConnectBlock`), not in the context-free
  header/block checks.
- Tip movement is serialized by `Chainstate::m_chainstate_mutex` inside
  `ActivateBestChain()`. The source comments warn callers not to invoke that
  path while holding `::cs_main`.
- `ChainstateManager` exposes several perspectives over its managed
  chainstates:
  - `CurrentChainstate()` is the chainstate currently targeted at the best
    network tip.
  - `HistoricalChainstate()` is the background full-validation chainstate when
    assumeutxo is active.
  - `ValidatedChainstate()` selects the fully validated view for consumers that
    must avoid assumed-valid state.
- Older code still exposes `ActiveChainstate()`-style naming, but
  `validation.h` comments explicitly steer newer code toward
  `CurrentChainstate()`.

## Important Code Paths

- Tip activation:
  `src/validation.cpp` (`Chainstate::ActivateBestChain`,
  `ActivateBestChainStep`, `ConnectTip`, `DisconnectTip`)
- UTXO validation:
  `src/validation.cpp` (`Chainstate::ConnectBlock`)
- Disk/cache management:
  `src/validation.cpp` (`FlushStateToDisk`, `ForceFlushStateToDisk`,
  `ReplayBlocks`, `LoadChainTip`)
- Initial construction and startup helpers:
  `src/node/chainstate.cpp`

## Related Tests

- `src/test/validation_chainstate_tests.cpp`
- `src/test/validation_chainstatemanager_tests.cpp`
- `src/test/validation_block_tests.cpp`
- `test/functional/feature_assumeutxo.py`

## Adjacent Pages

- `[[areas/validation-and-chainstate]]`
- `[[concepts/assumeutxo]]`
- `[[workflows/block-validation-and-connection]]`
- `[[files/src/validation.cpp]]`

## Sources Consulted

- `src/validation.h`
- `src/validation.cpp`
- `src/node/chainstate.cpp`
- `src/test/validation_chainstate_tests.cpp`
- `src/test/validation_chainstatemanager_tests.cpp`
