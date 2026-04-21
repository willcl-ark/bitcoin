---
kind: investigation
title: Critical Codepaths Priority Map
status: active
last_reviewed: 2026-04-21
tags:
  - critical
  - priority-map
  - review
---

# Critical Codepaths Priority Map

## Summary

This page turns the `AGENTS.md` critical-priority rubric into a concrete map of
where to deepen the wiki first. The focus is on code paths where failures could
crash the node, take it offline, cause OOM/resource exhaustion, lose funds, or
harm operator or transaction privacy.

## Priority Map

- Crash / fatal shutdown surfaces:
  - `src/validation.cpp` (`ProcessNewBlock`, `ConnectBlock`,
    `ActivateBestChain`)
  - `src/net.cpp` (`ThreadSocketHandler`, `ThreadMessageHandler`)
  - `src/node/miner.cpp` (`CreateNewBlock` self-validation path)
  - Tests: `src/test/validation_*`, `src/test/net_*`, `src/test/miner_tests.cpp`
- Offline / service-loss surfaces:
  - `src/net.cpp` (`OpenNetworkConnection`, `AcceptConnection`,
    `AttemptToEvictConnection`)
  - `src/net_processing.cpp` (`ProcessHeadersMessage`, `ProcessBlock`,
    `MaybeDiscourageAndDisconnect`)
  - `src/httprpc.cpp` (`HTTPReq_JSONRPC`, `StartHTTPRPC`)
  - Tests: `test/functional/p2p_*`, `test/functional/interface_http.py`,
    `test/functional/interface_rpc.py`
- OOM / resource-exhaustion surfaces:
  - `src/txmempool.cpp` (`TrimToSize`, `DynamicMemoryUsage`, `Expire`,
    `GetMinFee`)
  - `src/net_processing.cpp` orphan/retry handling
  - `src/net.cpp` socket/connection loops
  - Tests: `src/test/mempool_tests.cpp`, `test/functional/mempool_limit.py`,
    `src/test/fuzz/tx_pool.cpp`
- Fund-loss / wallet-safety surfaces:
  - `src/wallet/spend.cpp` (`CreateTransaction`, coin selection, change logic)
  - `src/wallet/wallet.cpp` wallet state and DB-backed transaction ownership
  - `src/wallet/load.cpp` / migration and startup flows
  - Tests: `src/wallet/test/spend_tests.cpp`,
    `src/wallet/test/coinselector_tests.cpp`,
    `test/functional/wallet_sendall.py`, `test/functional/wallet_bumpfee.py`
- Operator-privacy surfaces:
  - `src/net.cpp` (`GetAddresses`, `GetAddressesUnsafe`, transport handling)
  - `src/net_processing.cpp` (`SetupAddressRelay`, handshake behavior)
  - `src/httprpc.cpp` and `src/wallet/rpc/util.cpp` (wallet URI routing and
    RPC ingress)
  - `src/addrman.*` and related peer-selection state
- Sender / receiver privacy surfaces:
  - `src/wallet/spend.cpp` (coin selection and change behavior)
  - `src/net_processing.cpp` tx relay, orphan retry, and package handling
  - descriptor- and wallet-metadata surfaces tied to RPC/GUI flows

## Why These Are Critical

- They sit directly on untrusted-input boundaries, wallet transaction
  construction, or long-lived resource ownership.
- They connect multiple subsystems, so a misunderstanding of boundaries can
  turn a local bug into an availability or privacy regression.
- They already have specialized tests, which makes them good candidates for
  deeper file pages and for identifying coverage gaps.

## Related Pages

- `[[concepts/operator-privacy]]`
- `[[concepts/transaction-sender-and-receiver-privacy]]`
- `[[concepts/resource-exhaustion-and-backpressure]]`
- `[[concepts/wallet-fund-safety]]`
- `[[files/src/validation.cpp]]`
- `[[files/src/net.cpp]]`
- `[[files/src/net_processing.cpp]]`
- `[[files/src/txmempool.cpp]]`
- `[[files/src/wallet/spend.cpp]]`
- `[[files/src/wallet/wallet.cpp]]`
- `[[files/src/httprpc.cpp]]`
- `[[files/src/node/miner.cpp]]`
- `[[areas/validation-and-chainstate]]`
- `[[areas/p2p-and-networking]]`
- `[[areas/mempool-and-policy]]`
- `[[areas/wallet]]`

## Open Questions

- Which privacy-sensitive surfaces deserve dedicated concept pages beyond the
  current `addrman` and wallet-spending coverage?
- Which resource-exhaustion paths are bounded only by policy/heuristics rather
  than hard structural limits?
- Which of the critical paths above have weak fuzz or functional coverage
  compared with their failure impact?

## Sources Consulted

- `AGENTS.md`
- `src/validation.cpp`
- `src/net.cpp`
- `src/net_processing.cpp`
- `src/txmempool.cpp`
- `src/wallet/spend.cpp`
- `src/httprpc.cpp`
- Existing wiki pages under `wiki/areas/`, `wiki/concepts/`, and
  `wiki/workflows/`
