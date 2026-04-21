---
kind: concept
title: Assumeutxo
status: active
last_reviewed: 2026-04-21
paths:
  - doc/design/assumeutxo.md
  - src/validation.h
  - src/validation.cpp
  - src/node/utxo_snapshot.cpp
  - src/rpc/blockchain.cpp
tags:
  - assumeutxo
  - snapshots
  - chainstate
---

# Assumeutxo

## Summary

Assumeutxo is the snapshot-based chainstate bootstrapping path. It lets a node
load a serialized UTXO set at a chainparams-supported base block, promote that
snapshot chainstate to current use, and continue background historical
validation with a separate chainstate until the snapshot is fully validated.

## Responsibilities and Invariants

- `src/rpc/blockchain.cpp` (`loadtxoutset`) is the operator-facing entry point
  for loading a snapshot file.
- `src/validation.cpp` (`ChainstateManager::ActivateSnapshot`) stages the new
  snapshot chainstate and rejects unsupported or conflicting snapshot states.
- `PopulateAndValidateSnapshot()` loads the coins snapshot into a separate
  chainstate and validates it against the referenced base block metadata before
  promotion.
- Snapshot state is tracked by `Chainstate::m_from_snapshot_blockhash` in
  `src/validation.h`. A null value means traditional IBD/full-validation
  origin; a populated value marks a snapshot-origin chainstate.
- While the snapshot chainstate is catching up to network tip, the historical
  chainstate continues full validation toward the snapshot base. Both share the
  common block index through `ChainstateManager`.
- `MaybeRebalanceCaches()` shifts cache toward the snapshot chainstate while it
  is the priority sync path, then returns more cache to the historical
  validator once the snapshot chainstate leaves IBD.
- On restart, `LoadAssumeutxoChainstate()` re-detects the snapshot chainstate
  from disk state and resumes the two-chainstate process.

## Important Code Paths

- Design and lifecycle overview:
  `doc/design/assumeutxo.md`
- Snapshot activation:
  `src/validation.cpp` (`ActivateSnapshot`, `PopulateAndValidateSnapshot`,
  `LoadAssumeutxoChainstate`, `MaybeRebalanceCaches`)
- Snapshot metadata persistence:
  `src/node/utxo_snapshot.cpp`
- RPC surface:
  `src/rpc/blockchain.cpp` (`loadtxoutset`, `getchainstates`)

## Related Tests

- `test/functional/feature_assumeutxo.py`
- `src/test/validation_chainstatemanager_tests.cpp`

## Adjacent Pages

- `[[concepts/chainstate]]`
- `[[areas/validation-and-chainstate]]`
- `[[workflows/initial-block-download]]`

## Sources Consulted

- `doc/design/assumeutxo.md`
- `src/validation.h`
- `src/validation.cpp`
- `src/node/utxo_snapshot.cpp`
- `src/rpc/blockchain.cpp`
- `src/test/validation_chainstatemanager_tests.cpp`
- `test/functional/feature_assumeutxo.py`
