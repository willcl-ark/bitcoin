---
kind: investigation
title: Critical Test Coverage Gaps
status: active
last_reviewed: 2026-04-21
tags:
  - critical
  - testing
  - coverage
  - review
---

# Critical Test Coverage Gaps

## Summary

- Fact: the current tree already gives `src/node/txdownloadman_impl.cpp` and
  `src/node/txorphanage.cpp` a relatively strong three-layer mix of unit,
  fuzz, and functional coverage through `src/test/txdownload_tests.cpp`,
  `src/test/orphanage_tests.cpp`, `src/test/fuzz/txdownloadman.cpp`,
  `src/test/fuzz/txorphan.cpp`, `test/functional/p2p_tx_download.py`, and
  `test/functional/p2p_orphan_handling.py`.
- Fact: the weaker spots I found are mostly boundary paths where persistence,
  queueing, or metadata exposure meet critical behavior:
  `src/validation.cpp` flush/prune paths, HTTP ingress in
  `src/httpserver.cpp` / `src/httprpc.cpp`, wallet state mutation in
  `src/wallet/wallet.cpp`, and socket-loop backpressure in `src/net.cpp`.
- Inference: compared with their impact, these paths are thinner either because
  only one test layer is present, or because the consulted tests exercise a
  lower-level helper while stopping short of the boundary function that
  actually fails closed, aborts, sheds work, or exposes metadata.

## Coverage Map

| Path | Impact | Existing coverage in the current tree | Observed gap or asymmetry |
| --- | --- | --- | --- |
| `src/validation.cpp` (`Chainstate::FlushStateToDisk`) and `src/node/blockstorage.cpp` (`FindFilesToPrune`, `UnlinkPrunedFiles`) | `crash`, `offline`, `resource` | `src/test/chainstate_write_tests.cpp`, `src/test/blockmanager_tests.cpp`, `test/functional/feature_pruning.py`, `test/functional/feature_abortnode.py` | Inference: the consulted tests cover flush scheduling, prune bookkeeping, and one fatal `DisconnectTip()` failure, but I did not find a unit, fuzz, or functional test that forces `CheckDiskSpace(...)` failure or other write-path failures inside `Chainstate::FlushStateToDisk()`. |
| `src/kernel/disconnected_transactions.cpp` (`DisconnectedBlockTransactions::LimitMemoryUsage`) and `src/validation.cpp` (`DisconnectTip`, `MaybeUpdateMempoolForReorg`) | `resource`, `offline` | `test/functional/mempool_updatefromblock.py`, `src/test/validation_chainstatemanager_tests.cpp` | Inference: the 20 MiB disconnect-pool bound and reorg reinsertion behavior are mostly carried by one functional test family; the only consulted unit coverage around `DisconnectTip()` explicitly says it is "not a realistic use", and I did not find a dedicated fuzz target for this reorg spillover boundary. |
| `src/httpserver.cpp` (`http_request_cb`), `src/httprpc.cpp` (`HTTPReq_JSONRPC`), and `src/wallet/rpc/util.cpp` (`GetWalletNameFromJSONRPCRequest`) | `offline`, `resource`, `operator-privacy` | `test/functional/interface_http.py`, `test/functional/interface_rpc.py`, `src/test/httpserver_tests.cpp`, `src/test/fuzz/http_request.cpp`, `src/wallet/test/wallet_rpc_tests.cpp`, `test/functional/wallet_multiwallet.py` | Inference: parser and request-wrapper coverage is stronger than dispatch coverage. The consulted tests exercise size limits, request parsing, queue overflow, and wallet routing, but I did not find a test that directly asserts the `http_request_cb()` logging path for `/wallet/<walletname>` requests or the combined allowlist/dispatch/queue-reject path in one place. |
| `src/wallet/wallet.cpp` (`CommitTransaction`, `MarkReplaced`, `SetSpentKeyState`) and `src/wallet/spend.cpp` (`AvailableCoins`) | `fund-loss`, `wrong-transaction`, `sender/receiver privacy` | `src/wallet/test/spend_tests.cpp`, `src/wallet/test/wallet_tests.cpp`, `src/wallet/test/psbt_wallet_tests.cpp`, `test/functional/wallet_bumpfee.py`, `test/functional/wallet_avoidreuse.py`, `test/functional/wallet_send.py`, `test/functional/wallet_migration.py` | Inference: transaction construction and PSBT update paths have clearer direct tests than post-construction wallet-state mutation. Replacement metadata and avoid-reuse state are verified mostly through end-to-end RPC behavior, and I did not find a dedicated fuzz target or focused unit test for `CommitTransaction()`, `MarkReplaced()`, or `SetSpentKeyState()`. |
| `src/net.cpp` (`SocketHandlerConnected`, `CNode::MarkReceivedMsgsForProcessing`, `CNode::PollMessage`, `CConnman::InactivityCheck`) | `offline`, `resource` | `test/functional/p2p_invalid_messages.py`, `test/functional/p2p_v2_misbehaving.py`, `src/test/net_tests.cpp`, `src/test/fuzz/process_message.cpp`, `src/test/fuzz/process_messages.cpp` | Inference: malformed-message and handshake coverage is good, but the consulted tests do not directly drive the receive-queue high-water path (`m_recv_flood_size`, `fPauseRecv`) or assert the `SocketHandlerConnected()` branch that stops reading when sends are not fully drained. That leaves the actual socket-loop backpressure behavior thinner than the parser coverage around it. |

## Notable Gaps

- `src/validation.cpp` (`Chainstate::FlushStateToDisk`) is a stronger crash and
  offline boundary than its direct failure coverage suggests.
  Fact: `src/test/chainstate_write_tests.cpp` checks periodic flush timing and
  flushes inside `ActivateBestChain()`. `src/test/blockmanager_tests.cpp` and
  `test/functional/feature_pruning.py` focus on prune bookkeeping, block data
  availability, and prune-mode behavior. `test/functional/feature_abortnode.py`
  covers a fatal reorg path caused by missing undo data.
  Inference: among consulted tests, low-disk failure at the
  `CheckDiskSpace(...)` calls in `Chainstate::FlushStateToDisk()` is still
  uncovered directly, even though that function is the current fatal boundary
  for "disk space is too low" shutdowns.

- Reorg spillover handling has a visible functional test, but not much deeper
  layering.
  Fact: `test/functional/mempool_updatefromblock.py` explicitly targets
  `MAX_DISCONNECTED_TX_POOL_BYTES` and reorg re-adding behavior. The only
  consulted unit coverage nearby is the snapshot-setup code in
  `src/test/validation_chainstatemanager_tests.cpp`, which comments that its
  `DisconnectTip()` use is not realistic.
  Inference: this is thinner than the surrounding mempool and orphanage areas,
  where multiple unit and fuzz targets exist. The reorg-memory bound is a
  resource-control surface, but the current tree mostly checks it from the top
  via one functional scenario.

- HTTP ingress has better request-shape coverage than boundary coverage.
  Fact: `test/functional/interface_http.py` covers header/body limits, idle
  timeout, chunked transfer, and pipelining. `test/functional/interface_rpc.py`
  checks `-rpcworkqueue=1` overflow and expects "Work queue depth exceeded".
  `src/test/httpserver_tests.cpp` and `src/test/fuzz/http_request.cpp` stay at
  URI/query parsing and `HTTPRequest` behavior. `src/test/threadpool_tests.cpp`
  and `src/test/fuzz/threadpool.cpp` cover the generic queue primitive.
  `src/wallet/test/wallet_rpc_tests.cpp` and
  `test/functional/wallet_multiwallet.py` cover wallet endpoint routing.
  Inference: `src/httpserver.cpp` (`http_request_cb`) is still only indirectly
  covered as the place where allowlist rejection, sanitized URI logging, path
  dispatch, and queue-depth rejection come together. I did not find a consulted
  test that combines `/wallet/<walletname>` routing with HTTP debug logging,
  even though that path is privacy-sensitive.

- Wallet construction is better covered than wallet-state mutation after
  construction.
  Fact: `src/wallet/test/spend_tests.cpp` directly exercises transaction
  creation edge cases, and `src/wallet/test/psbt_wallet_tests.cpp` directly
  exercises `FillPSBT()`. `test/functional/wallet_bumpfee.py` checks
  `replaced_by_txid` / `replaces_txid` behavior, `test/functional/wallet_avoidreuse.py`
  checks avoid-reuse-visible effects, and `test/functional/wallet_migration.py`
  checks that replacement metadata survives migration. `src/wallet/test/wallet_tests.cpp`
  has only a simple `CommitTransaction()` path.
  Inference: for fund-safety and privacy-sensitive bookkeeping,
  `CommitTransaction()`, `MarkReplaced()`, and `SetSpentKeyState()` rely more
  on indirect functional evidence than on focused lower-level tests.

- Socket-loop backpressure looks thinner than the surrounding message-parsing
  coverage.
  Fact: `test/functional/p2p_invalid_messages.py` and
  `test/functional/p2p_v2_misbehaving.py` cover oversize packets, invalid
  message structure, and inactivity timeouts. `src/test/net_tests.cpp` has
  transport-focused coverage, and `src/test/fuzz/process_message.cpp` /
  `src/test/fuzz/process_messages.cpp` fuzz `ProcessMessagesOnce()`.
  Inference: I did not find a consulted test that explicitly drives
  `CNode::MarkReceivedMsgsForProcessing()` above `m_recv_flood_size` or checks
  that `CConnman::SocketHandlerConnected()` suppresses additional reads after a
  partial send drain. Those are the concrete resource-shedding branches in the
  live socket loop.

## Adjacent Pages

- `[[investigations/critical-codepaths-priority-map]]`
- `[[areas/testing]]`
- `[[concepts/resource-exhaustion-and-backpressure]]`
- `[[concepts/wallet-fund-safety]]`
- `[[concepts/operator-privacy]]`
- `[[workflows/node-startup-and-shutdown]]`
- `[[files/src/validation.cpp]]`
- `[[files/src/net.cpp]]`
- `[[files/src/httprpc.cpp]]`
- `[[files/src/wallet/wallet.cpp]]`

## Sources Consulted

- `AGENTS.md`
- `wiki/investigations/critical-codepaths-priority-map.md`
- `wiki/concepts/resource-exhaustion-and-backpressure.md`
- `wiki/concepts/wallet-fund-safety.md`
- `wiki/concepts/operator-privacy.md`
- `src/validation.cpp`
- `src/kernel/disconnected_transactions.cpp`
- `src/kernel/disconnected_transactions.h`
- `src/node/blockstorage.cpp`
- `src/httpserver.cpp`
- `src/httprpc.cpp`
- `src/wallet/rpc/util.cpp`
- `src/wallet/spend.cpp`
- `src/wallet/wallet.cpp`
- `src/net.cpp`
- `src/test/chainstate_write_tests.cpp`
- `src/test/blockmanager_tests.cpp`
- `src/test/validation_chainstatemanager_tests.cpp`
- `src/test/httpserver_tests.cpp`
- `src/test/net_tests.cpp`
- `src/test/threadpool_tests.cpp`
- `src/test/fuzz/http_request.cpp`
- `src/test/fuzz/threadpool.cpp`
- `src/test/fuzz/process_message.cpp`
- `src/test/fuzz/process_messages.cpp`
- `src/wallet/test/spend_tests.cpp`
- `src/wallet/test/psbt_wallet_tests.cpp`
- `src/wallet/test/wallet_tests.cpp`
- `src/wallet/test/wallet_rpc_tests.cpp`
- `src/test/txdownload_tests.cpp`
- `src/test/orphanage_tests.cpp`
- `src/test/fuzz/txdownloadman.cpp`
- `src/test/fuzz/txorphan.cpp`
- `test/functional/feature_abortnode.py`
- `test/functional/feature_pruning.py`
- `test/functional/mempool_updatefromblock.py`
- `test/functional/interface_http.py`
- `test/functional/interface_rpc.py`
- `test/functional/wallet_multiwallet.py`
- `test/functional/wallet_bumpfee.py`
- `test/functional/wallet_avoidreuse.py`
- `test/functional/wallet_send.py`
- `test/functional/wallet_migration.py`
- `test/functional/p2p_invalid_messages.py`
- `test/functional/p2p_v2_misbehaving.py`
- `test/functional/p2p_tx_download.py`
- `test/functional/p2p_orphan_handling.py`
