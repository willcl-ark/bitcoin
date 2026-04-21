---
kind: area
title: P2P and Networking
status: active
last_reviewed: 2026-04-21
paths:
  - src/net.cpp
  - src/net.h
  - src/net_processing.cpp
  - src/addrman.cpp
  - src/addrman.h
  - src/banman.cpp
  - src/banman.h
  - src/txrequest.cpp
  - src/txrequest.h
  - src/node/txdownloadman.h
  - src/node/txdownloadman_impl.cpp
  - src/node/txorphanage.h
  - src/node/txorphanage.cpp
tags:
  - p2p
  - networking
---

## Summary

This area owns peer connections, transport framing, handshake and message processing, headers/block/transaction relay, address gossip and persistence, peer discouragement, and transaction download scheduling. The main split is `src/net.*` for connection and transport machinery (`CConnman`, `CNode`, `V1Transport`, `V2Transport`) and `src/net_processing.cpp` for protocol-state logic (`PeerManagerImpl`).

## Responsibilities / Invariants

- `src/net.h` / `src/net.cpp` (`CConnman`) owns sockets, `CNode` lifecycle, connection slot accounting, inbound eviction, manual/seed/addr-fetch/private-broadcast connection management, and the thread split between `ThreadSocketHandler` and `ThreadMessageHandler`.
- `src/net.h` (`CNode`) keeps send and receive transport state separately locked. `src/net.h` / `src/net.cpp` (`V2Transport`) implements the BIP324 state machines and can fall back to `V1Transport` for incoming legacy peers; `src/net.cpp` (`CreateNodeFromAcceptedSocket`) enables v2 automatically for inbounds when local services include `NODE_P2P_V2`.
- `src/net_processing.cpp` (`PeerManagerImpl`) implements `NetEventsInterface` and owns handshake rules, per-peer protocol state, headers and block sync, transaction relay, compact blocks, and the boundary from p2p messages into chainstate and mempool calls.
- `src/addrman.h` (`AddrMan`) is the outbound address book. New addresses enter through `Add`, move to tried through `Good`, record dial attempts through `Attempt`, and are selected through bucketized, keyed randomness rather than straightforward LRU logic.
- `src/addrman.h` (`AddrMan::Connected`) is called when a connection ends, not when it starts, to avoid leaking information about currently connected peers into address timestamps that can later be gossiped.
- `src/banman.h` (`BanMan`) separates manual bans from automatic discouragement. Discouragement is bloom-filter based, cannot enumerate its contents, and the header comment explicitly notes it is not a generic DoS defense.
- `src/net_processing.cpp` (`m_tx_download_mutex`, `m_txdownloadman`) documents invariants tying together tx request tracking, rejection filters, confirmed-transaction filters, and orphan handling. `src/node/txdownloadman.h` notes that `TxDownloadManager` itself is not thread-safe and must be externally synchronized.
- `src/txrequest.h` (`TxRequestTracker`) models each peer/txhash announcement as candidate, requested, or completed, and keeps completed announcements only as short-lived suppression state so the same transaction is not immediately re-requested.
- `src/node/txorphanage.h` (`TxOrphanage`) is the bounded missing-input
  transaction store. It attributes orphans to announcer peers, tracks
  parent-to-child wakeups, and trims by deduplicated weight and latency score
  rather than by simple FIFO.
- `src/net_processing.cpp` (`SetupAddressRelay`) disables addr relay on outbound block-relay-only peers. `src/net.h` / `src/net.cpp` (`GetAddresses`, `GetAddressesUnsafe`) distinguish cached untrusted address responses from uncached trusted callers.

## Important Code Paths

- Outbound dialing runs through `src/net.cpp` (`CConnman::OpenNetworkConnection`, `CConnman::ConnectNode`). Inbound acceptance runs through `src/net.cpp` (`CConnman::AcceptConnection`, `CConnman::CreateNodeFromAcceptedSocket`), with `CConnman::AttemptToEvictConnection` and `BanMan::IsDiscouraged` influencing whether a new inbound is admitted or preferred for eviction.
- Socket and message handling are split: `src/net.cpp` (`CConnman::ThreadSocketHandler`) handles IO and queueing, while `src/net.cpp` (`CConnman::ThreadMessageHandler`) randomizes peer order and then calls `ProcessMessages` and `SendMessages` on the active `NetEventsInterface`.
- Handshake and relay setup are centered in `src/net_processing.cpp` (`PeerManagerImpl::ProcessMessage`) for `VERSION` and `VERACK`: service checks, self-connection detection, `WTXIDRELAY`, `SENDADDRV2`, optional `SENDTXRCNCL`, addr-relay setup, and successful outbound promotion in `AddrMan::Good`.
- Header and block sync are driven by `src/net_processing.cpp` (`MaybeSendGetHeaders`, `ProcessHeadersMessage`, `HeadersDirectFetchBlocks`, `FetchBlock`, `ProcessBlock`). `ProcessBlock` hands accepted blocks to `ChainstateManager::ProcessNewBlock`.
- Transaction relay flows through `src/node/txdownloadman.h` / `src/node/txdownloadman_impl.cpp` (`AddTxAnnouncement`, `GetRequestsToSend`, `ReceivedTx`, `MempoolAcceptedTx`, `MempoolRejectedTx`) and `src/net_processing.cpp` (`ProcessValidTx`, `ProcessInvalidTx`, `ProcessOrphanTx`).
- Missing-input retry and orphan pressure are centered in
  `src/node/txorphanage.cpp` (`AddTx`, `LimitOrphans`,
  `AddChildrenToWorkSet`, `GetTxToReconsider`, `EraseForBlock`,
  `EraseForPeer`) and called from `src/node/txdownloadman_impl.cpp`.
- Incoming address gossip is filtered and rate-limited in `src/net_processing.cpp` (`ProcessAddrs`) before storage through `AddrMan::Add`. Outgoing addr responses use the per-requestor cache in `src/net.cpp` (`CConnman::GetAddresses`) unless a trusted caller explicitly uses `GetAddressesUnsafe`.

## Related Tests

- Unit: `src/test/net_tests.cpp`, `src/test/net_peer_connection_tests.cpp`, `src/test/net_peer_eviction_tests.cpp`, `src/test/peerman_tests.cpp`, `src/test/addrman_tests.cpp`, `src/test/banman_tests.cpp`, `src/test/txrequest_tests.cpp`, `src/test/txdownload_tests.cpp`, `src/test/orphanage_tests.cpp`, `src/test/headers_sync_chainwork_tests.cpp`, `src/test/bip324_tests.cpp`, `src/test/private_broadcast_tests.cpp`.
- Functional: `test/functional/p2p_handshake.py`, `test/functional/p2p_add_connections.py`, `test/functional/p2p_headers_sync_with_minchainwork.py`, `test/functional/p2p_initial_headers_sync.py`, `test/functional/p2p_block_sync.py`, `test/functional/p2p_addr_relay.py`, `test/functional/p2p_getaddr_caching.py`, `test/functional/p2p_tx_download.py`, `test/functional/p2p_orphan_handling.py`, `test/functional/p2p_disconnect_ban.py`, `test/functional/p2p_outbound_eviction.py`, `test/functional/p2p_v2_transport.py`, `test/functional/p2p_v2_encrypted.py`, `test/functional/feature_addrman.py`, `test/functional/p2p_private_broadcast.py`.
- Fuzz: `src/test/fuzz/p2p_handshake.cpp`, `src/test/fuzz/bip324.cpp`, `src/test/fuzz/addrman.cpp`, `src/test/fuzz/banman.cpp`, `src/test/fuzz/txrequest.cpp`, `src/test/fuzz/txdownloadman.cpp`, `src/test/fuzz/txorphan.cpp`.

## Adjacent Pages

- [[areas/validation-and-chainstate]]
- [[areas/mempool-and-policy]]
- [[concepts/addrman]]
- [[concepts/package-policy-and-relay]]
- [[concepts/operator-privacy]]
- [[concepts/resource-exhaustion-and-backpressure]]
- [[files/src/addrman.cpp]]
- [[files/src/net.cpp]]
- [[files/src/net_processing.cpp]]
- [[files/src/node/txdownloadman_impl.cpp]]
- [[files/src/node/txorphanage.cpp]]
- [[investigations/critical-codepaths-priority-map]]
- [[workflows/block-relay]]
- [[workflows/initial-block-download]]
- [[workflows/block-validation-and-connection]]

## Sources Consulted

- `src/net.h` (`CNode`, `V1Transport`, `V2Transport`, `CConnman`)
- `src/net.cpp` (`CConnman::ThreadSocketHandler`, `CConnman::ThreadMessageHandler`, `CConnman::CreateNodeFromAcceptedSocket`, `CConnman::GetAddresses`, `CConnman::GetAddressesUnsafe`)
- `src/net_processing.cpp` (`PeerManagerImpl`, `ProcessMessage`, `SendMessages`, `MaybeDiscourageAndDisconnect`, `ProcessHeadersMessage`, `ProcessBlock`, `ProcessValidTx`, `ProcessInvalidTx`, `ProcessOrphanTx`, `SetupAddressRelay`, `ProcessAddrs`)
- `src/addrman.h` (`AddrMan`)
- `src/banman.h` (`BanMan`)
- `src/txrequest.h` (`TxRequestTracker`)
- `src/node/txdownloadman.h` (`TxDownloadManager`)
- `src/node/txdownloadman_impl.cpp` (`TxDownloadManagerImpl`)
