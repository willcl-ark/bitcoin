---
kind: area
title: Mining and Block Assembly
status: active
last_reviewed: 2026-04-21
paths:
  - src/node/miner.cpp
  - src/node/miner.h
  - src/rpc/mining.cpp
  - src/interfaces/mining.h
tags:
  - mining
  - block-assembly
  - getblocktemplate
---

# Mining and Block Assembly

## Summary

Bitcoin Core's mining area assembles candidate blocks from the active
chainstate and current mempool, exposes block-template APIs to internal callers
and RPC clients, and validates submitted blocks against the current tip before
acceptance. The main implementation lives in `src/node/miner.*`
(`BlockAssembler`, `WaitAndCreateNewBlock`), with RPC entry points in
`src/rpc/mining.cpp` and an internal library boundary in
`src/interfaces/mining.h`.

## Responsibilities and Invariants

- `src/node/miner.cpp` (`BlockAssembler::CreateNewBlock`) builds a
  `CBlockTemplate` from the active chainstate and mempool. It tracks block
  weight, sigops, package feerates, and total fees while respecting assembler
  options such as block weight and minimum feerate.
- `src/node/miner.h` (`ApplyArgsManOptions`) maps runtime options like
  `-blockmintxfee` and `-blockmaxweight` into `BlockAssembler::Options`.
- `CreateNewBlock()` can self-check the finished candidate with
  `TestBlockValidity()` before returning. This makes template construction a
  policy/mining surface, but one that still reuses current consensus and
  chainstate validation code before handing a block to miners or tests.
- `src/node/miner.cpp` (`WaitAndCreateNewBlock`) is the long-poll style helper
  that waits for chain or mempool changes and rebuilds a template when the old
  one is stale.
- `src/interfaces/mining.h` abstracts mining consumers away from RPC-specific
  code. `interfaces::Mining::createNewBlock()` and `interfaces::BlockTemplate`
  let library users create, update, and submit templates without going through
  JSON-RPC.
- RPC mining commands are not all equivalent:
  - `getblocktemplate` is the BIP22/BIP23-facing template API.
  - `submitblock` submits a serialized block to validation and reports the
    result through `submitblock_StateCatcher`.
  - `generateblock` is a hidden helper that constructs a block, adds requested
    transactions, and validates the result.
  - `getmininginfo` is a read-only inspection surface.

## Important Code Paths

- Template assembly:
  `src/node/miner.cpp` (`BlockAssembler::CreateNewBlock`, `addChunks`,
  `AddToBlock`, `TestChunkBlockLimits`, `TestChunkTransactions`)
- Wait-and-refresh template flow:
  `src/node/miner.cpp` (`WaitAndCreateNewBlock`)
- Internal interface wrapper:
  `src/node/interfaces.cpp` (`BlockTemplateImpl`, `createNewBlock`,
  `waitNext`, `submit`)
- Mining RPCs:
  `src/rpc/mining.cpp` (`getmininginfo`, `getblocktemplate`, `submitblock`,
  `generateblock`)

## Related Tests

- Unit: `src/test/miner_tests.cpp`
- Functional: `test/functional/mining_basic.py`,
  `test/functional/mining_template_verification.py`,
  `test/functional/mining_getblocktemplate_longpoll.py`,
  `test/functional/mining_prioritisetransaction.py`,
  `test/functional/mining_mainnet.py`

## Adjacent Pages

- `[[areas/mempool-and-policy]]`
- `[[areas/validation-and-chainstate]]`
- `[[areas/rpc-rest-zmq-and-interfaces]]`
- `[[workflows/block-validation-and-connection]]`

## Sources Consulted

- `src/node/miner.h`
- `src/node/miner.cpp`
- `src/interfaces/mining.h`
- `src/node/interfaces.cpp`
- `src/rpc/mining.cpp`
- `src/test/miner_tests.cpp`
- `test/functional/mining_basic.py`
- `test/functional/mining_template_verification.py`
- `test/functional/mining_getblocktemplate_longpoll.py`
