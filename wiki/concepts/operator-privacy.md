---
kind: concept
title: Operator Privacy
status: active
last_reviewed: 2026-04-21
paths:
  - src/init.cpp
  - src/init/common.cpp
  - src/net.cpp
  - src/net_processing.cpp
  - src/netbase.h
  - src/netbase.cpp
  - src/node/connection_types.h
  - src/httpserver.cpp
  - src/httprpc.cpp
  - src/wallet/rpc/util.cpp
  - src/rpc/net.cpp
  - src/torcontrol.cpp
  - src/bip324.h
  - src/bip324.cpp
tags:
  - privacy
  - operator
  - p2p
  - rpc
---

# Operator Privacy

## Summary

This page tracks operator-privacy properties of the current Bitcoin Core tree:
what the node does to avoid exposing the operator's network location or peer
graph, and where current behavior still reveals metadata. The main impact types
are `operator-IP exposure`, `topology correlation`, and
`operator fingerprinting`.

Facts in the current tree point in both directions. `src/net_processing.cpp`
(`SetupAddressRelay`) disables addr relay on outbound block-relay-only peers,
`src/net.cpp` (`GetAddresses`) caches `getaddr` responses for roughly a day,
and `src/netbase.h` / `src/netbase.cpp` (`SetNameProxy`, `ConnectThroughProxy`)
support proxy-based DNS and Tor stream isolation. At the same time,
`src/httpserver.cpp` can log request URIs and client addresses, wallet routing
uses `/wallet/<walletname>` via `src/wallet/rpc/util.cpp`
(`GetWalletNameFromJSONRPCRequest`), and enabling `-logips`,
`-capturemessages`, or net tracepoints creates durable local metadata.

## Invariants

- Fact: local-address self-announcements are network-scoped rather than
  global. `src/net.cpp` (`GetLocal`) refuses to advertise a privacy-network
  address to a non-privacy peer, and refuses the reverse as well.
  `src/net_processing.cpp` (`MaybeSendAddr`) only self-announces when
  `fListen` is true and initial block download has finished. Privacy impact:
  `operator-IP exposure`.
- Fact: outbound block-relay-only peers do not participate in addr relay.
  `src/net_processing.cpp` (`SetupAddressRelay`) returns false for
  `node.IsBlockOnlyConn()` specifically to avoid letting addr traffic reveal
  the link. Privacy impact: `topology correlation`.
- Fact: outbound peer selection is intentionally randomized and diversified
  when Core is using addrman. `src/addrman.h` (`AddrMan::Select`) and
  `src/net.cpp` (`ThreadOpenConnections`, `MaybePickPreferredNetwork`) avoid
  repeatedly choosing the same IPv4/IPv6 netgroups and try to maintain at
  least one outbound peer per reachable network. Manual peers from `-connect`
  or `addnode` bypass that randomness via `src/net.cpp`
  (`ThreadOpenConnections`, `AddNode`, `OpenNetworkConnection`). Privacy
  impact: `topology correlation` and `operator fingerprinting`.
- Fact: address export is deliberately damped to make scraping and timestamp
  inference harder. `src/net.cpp` (`GetAddresses`) caches responses for
  `21h + rand(0..6h)` per requestor `m_network_key`, and `src/addrman.h`
  documents that `AddrMan::Connected` is called on disconnect so addr
  timestamps are not updated while the peer is still connected. Privacy
  impact: `topology correlation`.
- Fact: DNS privacy depends on proxy configuration. `src/netbase.h`
  (`SetNameProxy`) documents that hostname resolution is delegated to the
  proxy instead of local DNS once a name proxy is set. `src/net.cpp`
  (`ConnectNode`) therefore resolves `pszDest` locally only when
  `fNameLookup && !HaveNameProxy()`, and `src/net.cpp`
  (`ThreadDNSAddressSeed`) switches from local DNS lookups to `AddAddrFetch`
  when a name proxy is available. Privacy impact: `operator-IP exposure`.
- Fact: Tor and I2P transport choices are treated as privacy-sensitive. `src/init.cpp`
  wires `-proxyrandomize` into `Proxy(..., tor_stream_isolation=true)`,
  `src/netbase.cpp` (`ConnectThroughProxy`) generates per-connection SOCKS5
  credentials when stream isolation is enabled, and `src/init.cpp` rejects
  `-privatebroadcast` with `-connect` while warning that `-proxyrandomize=0`
  allows Tor circuit correlation. `src/net.cpp` (`Bind`) uses
  `BF_DONT_ADVERTISE` for onion binds, and `src/torcontrol.cpp`
  (`TorController::add_onion_cb`) uses the default P2P port for automatically
  created onion services "to avoid decloaking nodes using other ports".
  Privacy impact: `operator-IP exposure` and `operator fingerprinting`.
- Fact: the RPC server defaults to loopback-only exposure. `src/httpserver.cpp`
  (`InitHTTPAllowList`, `HTTPBindAddresses`) always allows localhost, ignores
  `-rpcbind` unless `-rpcallowip` is also set, and warns when the RPC server
  is bound broadly. Privacy impact: `operator-IP exposure`.
- Fact: network RPCs expose operator and peer metadata when they are reachable.
  `src/rpc/net.cpp` (`getpeerinfo`) returns peer addresses, `addrlocal`,
  `connection_type`, `transport_protocol_type`, and `session_id`;
  `src/rpc/net.cpp` (`getnetworkinfo`) returns proxy settings and
  `localaddresses`; and `src/rpc/net.cpp` (`getnodeaddresses`) exports addrman
  records. Privacy impact: `operator-IP exposure` and `topology correlation`.
- Fact: wallet selection is encoded in the URI path. `src/httprpc.cpp`
  (`HTTPReq_JSONRPC`) stores `req->GetURI()` in `JSONRPCRequest::URI`, and
  `src/wallet/rpc/util.cpp` (`GetWalletNameFromJSONRPCRequest`) extracts the
  wallet name from `/wallet/<walletname>`. If HTTP request logging is enabled,
  `src/httpserver.cpp` (`http_request_cb`) logs the sanitized URI and client
  address before dispatch. Privacy impact: `operator fingerprinting`.
- Fact: several local observability features write sensitive metadata. `src/init/common.cpp`
  (`AddLoggingArgs`) exposes `-logips`; `src/net.cpp` and
  `src/net_processing.cpp` log or capture peer addresses when `fLogIPs` or
  `m_capture_messages` are enabled; `src/net.cpp` (`CaptureMessageToFile`)
  writes per-peer `msgs_recv.dat` / `msgs_sent.dat` under
  `message_capture/<peer>/`; and `TRACEPOINT(net, ...)` callsites in
  `src/net.cpp` and `src/net_processing.cpp` publish peer addresses,
  connection types, and in the message tracepoints the message bytes
  themselves. Privacy impact: `operator-IP exposure`, `topology correlation`,
  and `operator fingerprinting`.
- Fact: peers can see operator-chosen version metadata. `src/init.cpp`
  builds `strSubVersion` from `-uacomment`, and `src/net_processing.cpp`
  (`PushNodeVersion`) sends that string in the `version` message. Privacy
  impact: `operator fingerprinting`.
- Inference: enabling BIP324 v2 reduces passive on-path visibility of P2P
  payload contents, but it does not hide the existence of a connection or the
  operator's network address from the remote peer. The code basis is
  `src/net.cpp` (`MakeTransport`, `V2Transport::ProcessReceivedMaybeV1Bytes`,
  `ThreadOpenConnections`) together with `src/bip324.h` / `src/bip324.cpp`
  (`BIP324Cipher`), which encrypt the transport once both sides negotiate v2
  and otherwise fall back to v1.

## Important code paths

- Local address advertisement and addr relay:
  `src/net.cpp` (`GetLocal`, `GetLocalAddrForPeer`, `AddLocal`, `Bind`),
  `src/net_processing.cpp` (`MaybeSendAddr`, `SetupAddressRelay`,
  `RelayAddress`, `ProcessAddrs`), `src/addrman.h` (`AddrMan::Connected`).
- Outbound peer selection, DNS seeding, and addr export:
  `src/net.cpp` (`ThreadOpenConnections`, `MaybePickPreferredNetwork`,
  `ConnectNode`, `ThreadDNSAddressSeed`, `ProcessAddrFetch`, `GetAddresses`,
  `GetAddressesUnsafe`), `src/netbase.h` (`SetNameProxy`), `src/netbase.cpp`
  (`ConnectThroughProxy`).
- Transport and private-broadcast behavior:
  `src/node/connection_types.h` (`ConnectionType`, `TransportProtocolType`),
  `src/net.cpp` (`MakeTransport`, `V2Transport::ProcessReceivedMaybeV1Bytes`,
  `ThreadPrivateBroadcast`, `PushMessage`), `src/bip324.h`
  (`BIP324Cipher`), `src/init.cpp` (proxy parsing and `-privatebroadcast`
  parameter checks), `src/torcontrol.cpp` (`TorController::add_onion_cb`).
- RPC ingress and metadata surfaces:
  `src/httpserver.cpp` (`InitHTTPAllowList`, `HTTPBindAddresses`,
  `http_request_cb`), `src/httprpc.cpp` (`HTTPReq_JSONRPC`),
  `src/wallet/rpc/util.cpp` (`GetWalletNameFromJSONRPCRequest`),
  `src/rpc/net.cpp` (`getpeerinfo`, `getnetworkinfo`, `getnodeaddresses`).

## Related tests

- `test/functional/p2p_addr_relay.py` checks addr relay enablement, relay fanout,
  and the block-relay-only exception.
- `test/functional/p2p_getaddr_caching.py` checks stable per-bind `getaddr`
  responses and later cache rotation.
- `test/functional/p2p_v2_transport.py`, `test/functional/p2p_v2_encrypted.py`,
  and `src/test/bip324_tests.cpp` cover v1/v2 negotiation, fallback, session
  IDs, and BIP324 cipher behavior.
- `test/functional/p2p_private_broadcast.py` and
  `src/test/private_broadcast_tests.cpp` cover private-broadcast connection
  selection and recipient tracking.
- `test/functional/interface_http.py`, `test/functional/rpc_net.py`, and
  `test/functional/wallet_multiwallet.py` cover HTTP ingress, network metadata
  RPCs, and wallet-endpoint routing requirements.

## Adjacent pages

- `[[areas/p2p-and-networking]]`
- `[[areas/rpc-rest-zmq-and-interfaces]]`
- `[[areas/wallet]]`
- `[[concepts/addrman]]`
- `[[concepts/rpc-authentication-and-wallet-routing]]`
- `[[workflows/rpc-request-handling]]`
- `[[files/src/addrman.cpp]]`
- `[[files/src/net.cpp]]`
- `[[files/src/net_processing.cpp]]`
- `[[files/src/httprpc.cpp]]`

## Open questions

- The current tree has strong mechanics for reducing addr and peer-selection
  leakage, but it still exposes rich local observability knobs. Should the wiki
  add an operator-facing page that treats `-logips`, `-capturemessages`,
  tracepoints, and HTTP debug logging as privacy-sensitive operational modes?
- Wallet routing still depends on `/wallet/<walletname>` in the URI path. If a
  deployment sits behind a reverse proxy or shared HTTP logging layer, that
  path is easy to retain outside Core. The current tree does not provide a
  path-free wallet selector for HTTP JSON-RPC.
- `-privatebroadcast` currently requires random peer selection from addrman and
  warns about Tor circuit reuse when `-proxyrandomize=0`. It remains worth
  checking, in future review work, whether any remaining local logs or RPC
  surfaces can correlate those short-lived connections with other activity.

## Sources consulted

- `src/init.cpp`
- `src/init/common.cpp`
- `src/net.cpp`
- `src/net_processing.cpp`
- `src/netbase.h`
- `src/netbase.cpp`
- `src/node/connection_types.h`
- `src/httpserver.cpp`
- `src/httprpc.cpp`
- `src/wallet/rpc/util.cpp`
- `src/rpc/net.cpp`
- `src/torcontrol.cpp`
- `src/bip324.h`
- `src/bip324.cpp`
- `src/addrman.h`
- `test/functional/p2p_addr_relay.py`
- `test/functional/p2p_getaddr_caching.py`
- `test/functional/p2p_private_broadcast.py`
- `test/functional/p2p_v2_transport.py`
- `test/functional/p2p_v2_encrypted.py`
- `test/functional/interface_http.py`
- `test/functional/rpc_net.py`
- `test/functional/wallet_multiwallet.py`
- `src/test/bip324_tests.cpp`
- `src/test/private_broadcast_tests.cpp`
