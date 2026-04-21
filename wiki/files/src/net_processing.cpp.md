---
kind: file
title: src/net_processing.cpp
status: active
last_reviewed: 2026-04-21
paths:
  - src/net_processing.cpp
tags:
  - p2p
  - relay
  - headers
---

# src/net_processing.cpp

## Role in the System

`src/net_processing.cpp` is the node's P2P protocol-state engine. It
implements handshake rules, header and block sync, transaction relay,
discouragement/disconnection behavior, address relay, and the boundary from
peer messages into mempool and validation calls.

## Important Types and Functions

- `PeerManagerImpl::ProcessMessages()` and `SendMessages()` are the main
  per-peer processing loops.
- `PeerManagerImpl::ProcessMessage()` dispatches individual P2P message types.
- `ProcessHeadersMessage()` advances header sync and related peer state.
- `ProcessBlock()` hands a received block toward validation.
- `ProcessValidTx()`, `ProcessInvalidTx()`, and `ProcessOrphanTx()` bridge P2P
  relay behavior into mempool/package acceptance results.
- `MaybeDiscourageAndDisconnect()` applies discouragement/disconnect outcomes.
- `SetupAddressRelay()` controls whether a peer participates in addr relay.

## Callers and Dependencies

- Called from `CConnman::ThreadMessageHandler()` through `NetEventsInterface`.
- Depends on `ChainstateManager`, the active mempool, `TxDownloadManager`,
  `BanMan`, `AddrMan`, orphan-handling state, and per-peer tracking structures.
- Feeds blocks to validation and transactions/packages into the mempool
  acceptance path.

## Related Tests

- `src/test/peerman_tests.cpp`
- `src/test/txdownload_tests.cpp`
- `src/test/headers_sync_chainwork_tests.cpp`
- `test/functional/p2p_handshake.py`
- `test/functional/p2p_headers_sync_with_minchainwork.py`
- `test/functional/p2p_ibd_txrelay.py`
- `test/functional/p2p_orphan_handling.py`
- `test/functional/p2p_addr_relay.py`
- `test/functional/p2p_blocksonly.py`

## Notes or Risks

- Critical categories:
  - `offline`: incorrect discouragement, sync-peer handling, or block/header
    processing can stall synchronization or isolate the node.
  - `resource`: orphan handling, transaction retries, and peer message loops
    are attacker-facing amplification surfaces.
  - `operator privacy`: addr relay, peer selection, and handshake feature
    negotiation leak information about node behavior.
  - `sender/receiver privacy`: relay behavior and transaction processing policy
    can leak transaction-origin hints or wallet-usage patterns.
- This file is a major choke point between untrusted peer input and internal
  state transitions. High-risk review often depends on understanding exactly
  which messages can allocate memory, trigger validation, or alter peer state.
- `MaybeDiscourageAndDisconnect()` and the orphan/package retry paths are
  especially sensitive because they combine DoS policy, relay correctness, and
  privacy implications.

## Sources Consulted

- `src/net_processing.cpp`
- `src/test/peerman_tests.cpp`
- `src/test/txdownload_tests.cpp`
- `src/test/headers_sync_chainwork_tests.cpp`
- `test/functional/p2p_handshake.py`
- `test/functional/p2p_headers_sync_with_minchainwork.py`
- `test/functional/p2p_ibd_txrelay.py`
- `test/functional/p2p_orphan_handling.py`
- `test/functional/p2p_addr_relay.py`
- `test/functional/p2p_blocksonly.py`
