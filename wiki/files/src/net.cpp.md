---
kind: file
title: src/net.cpp
status: active
last_reviewed: 2026-04-21
paths:
  - src/net.cpp
tags:
  - networking
  - p2p
  - transport
---

# src/net.cpp

## Role in the System

`src/net.cpp` is the transport and connection-management implementation for the
node's P2P network stack. It owns socket acceptance, outbound dialing, `CNode`
lifecycle, inbound eviction, address-response caching, and the split between
the low-level socket thread and the message-processing thread.

## Important Types and Functions

- `CConnman::ThreadSocketHandler()` manages socket IO and queue servicing.
- `CConnman::ThreadMessageHandler()` hands queued peer traffic to the current
  `NetEventsInterface` implementation.
- `CConnman::AcceptConnection()` and
  `CConnman::CreateNodeFromAcceptedSocket()` create inbound peers and apply
  admission/eviction policy.
- `CConnman::OpenNetworkConnection()` is the main outbound connection path.
- `CConnman::AttemptToEvictConnection()` selects an inbound for eviction when
  capacity is exhausted.
- `CConnman::GetAddresses()` and `GetAddressesUnsafe()` expose addr-response
  behavior with and without the per-requestor cache.
- `V1Transport` / `V2Transport` and `CNode` state are declared in `src/net.h`
  and used throughout this file.

## Callers and Dependencies

- Called by startup/shutdown and networking thread setup in the node runtime.
- Calls into the active `NetEventsInterface` implementation, which is
  currently `PeerManagerImpl` in `src/net_processing.cpp`.
- Depends on `AddrMan`, `BanMan`, `NetGroupManager`, permissions logic, socket
  wrappers, and transport implementations declared in `src/net.h`.

## Related Tests

- `src/test/net_tests.cpp`
- `src/test/net_peer_connection_tests.cpp`
- `src/test/net_peer_eviction_tests.cpp`
- `src/test/bip324_tests.cpp`
- `test/functional/p2p_add_connections.py`
- `test/functional/p2p_v2_transport.py`
- `test/functional/p2p_getaddr_caching.py`
- `test/functional/p2p_outbound_eviction.py`

## Notes or Risks

- Critical categories:
  - `offline`: bugs here can stop inbound acceptance, break outbound dialing,
    or deadlock/disable the network threads.
  - `resource`: socket loops, reconnection loops, and inbound slot handling are
    direct CPU/fd/bandwidth pressure surfaces.
  - `operator privacy`: transport selection, `getaddr` caching behavior, and
    peer-admission logic affect what remote peers can infer about the node.
- `ThreadSocketHandler()` and `ThreadMessageHandler()` form a hard availability
  boundary. Regressions here can wedge the node without touching higher-level
  validation logic.
- `GetAddressesUnsafe()` is explicitly documented in `src/net.h` as unsafe for
  untrusted callers because it bypasses the response cache.
- Inbound eviction and accepted-socket creation are high-risk review surfaces
  because mistakes can either lock out honest peers or make the node easier to
  eclipse or resource-exhaust.

## Sources Consulted

- `src/net.h`
- `src/net.cpp`
- `src/test/net_tests.cpp`
- `src/test/net_peer_connection_tests.cpp`
- `src/test/net_peer_eviction_tests.cpp`
- `src/test/bip324_tests.cpp`
- `test/functional/p2p_add_connections.py`
- `test/functional/p2p_v2_transport.py`
- `test/functional/p2p_getaddr_caching.py`
- `test/functional/p2p_outbound_eviction.py`
