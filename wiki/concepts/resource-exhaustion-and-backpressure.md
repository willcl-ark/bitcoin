---
kind: concept
title: Resource Exhaustion and Backpressure
status: active
last_reviewed: 2026-04-21
paths:
  - src/net.cpp
  - src/net_processing.cpp
  - src/node/txdownloadman_impl.cpp
  - src/node/txorphanage.cpp
  - src/txmempool.cpp
  - src/validation.cpp
  - src/httpserver.cpp
tags:
  - dos
  - resource-limits
  - backpressure
---

# Resource Exhaustion and Backpressure

## Summary

Bitcoin Core does not have one node-wide resource governor. Current defenses are
mostly local: transport-level buffering and disconnects in `src/net.cpp`,
relay/orphan caps in `src/node/txdownloadman_impl.cpp` and
`src/node/txorphanage.cpp`, mempool trimming in `src/validation.cpp` and
`src/txmempool.cpp`, prune-mode disk shedding in `src/node/blockstorage.cpp`,
and RPC queue rejection in `src/httpserver.cpp`.

Hard limits are used for message sizes, peer/request counts, queue depths, and
some memory pools. Heuristics or policy are used for eviction choice, delayed
transaction requesting, rolling mempool fee floors, and prune scheduling. These
mechanisms are primarily availability defenses: they try to avoid OOM or
unbounded queue/file growth by disconnecting peers, dropping data, refusing new
work, or failing fast on low disk space.

## Invariants

- Automatic peer/socket count is bounded by `-maxconnections` in `src/init.cpp`,
  which feeds `src/net.h` (`CConnman::Init`). When inbound slots are full,
  `src/net.cpp` (`CConnman::AcceptConnection`,
  `CConnman::AttemptToEvictConnection`) evicts or drops peers instead of
  growing fd usage. Manual `-addnode` connections and private-broadcast
  connections have separate caps.
- Each peer has bounded transport state. `src/net.cpp` (`V1Transport::readHeader`)
  rejects payloads above `MAX_SIZE` or `MAX_PROTOCOL_MESSAGE_LENGTH` before
  allocating the full receive buffer; the source comment notes the older
  per-connection 32 MiB allocation/OOM issue. `src/net.cpp` / `src/net.h`
  (`CNode::MarkReceivedMsgsForProcessing`, `CNode::PollMessage`, `fPauseRecv`,
  `fPauseSend`) stop reading or pause sending once per-peer queue or send memory
  crosses `m_recv_flood_size` or `nSendBufferMaxSize`.
- Backpressure in the socket loop is explicit. `src/net.cpp`
  (`CConnman::SocketHandlerConnected`) tries to drain pending sends before
  reading more, and if sends still have `data_left` it suppresses additional
  reads for that iteration so TCP flow control can work. `CConnman::InactivityCheck`
  disconnects peers that stop making progress.
- Message fanout is also capped. `src/net_processing.cpp` rejects incoming
  `INV` and `GETDATA` messages above `MAX_INV_SZ`, rejects `HEADERS` messages
  above `m_opts.max_headers_result`, batches outgoing `GETDATA` to
  `MAX_GETDATA_SZ`, and limits block download concurrency to
  `MAX_BLOCKS_IN_TRANSIT_PER_PEER`.
- Transaction relay state is bounded per peer. `src/node/txdownloadman.h` fixes
  `MAX_PEER_TX_ANNOUNCEMENTS = 5000` and uses
  `MAX_PEER_TX_REQUEST_IN_FLIGHT = 100` as an overload threshold.
  `src/node/txdownloadman_impl.cpp` (`AddTxAnnouncement`,
  `MaybeAddOrphanResolutionCandidate`) drops or delays more work once those
  thresholds are hit.
- The orphanage is bounded by both memory and latency. `src/node/txorphanage.h`
  sets `DEFAULT_RESERVED_ORPHAN_WEIGHT_PER_PEER = 404000` and
  `DEFAULT_MAX_ORPHANAGE_LATENCY_SCORE = 3000`; `src/node/txorphanage.cpp`
  (`NeedsTrim`, `LimitOrphans`) trims until both global usage and global
  latency score are back under their computed caps. Large orphans above
  `MAX_STANDARD_TX_WEIGHT` are not stored at all in
  `TxOrphanageImpl::AddTx`.
- Mempool memory is hard-bounded after admission by `src/validation.cpp`
  (`LimitMempoolSize`) and `src/txmempool.cpp` (`CTxMemPool::TrimToSize`). The
  steady-state target is `kernel::MemPoolOptions::max_size_bytes`;
  `CTxMemPool::Expire` also drops old entries. This is an OOM boundary, but
  accepted packages can temporarily overshoot before post-submit trimming
  because `src/validation.cpp` (`MemPoolAccept::SubmitPackage`) explicitly
  skips `LimitMempoolSize()` until after submission.
- Reorg spillover is separately bounded. `src/kernel/disconnected_transactions.h`
  sets `MAX_DISCONNECTED_TX_POOL_BYTES = 20'000'000`;
  `DisconnectedBlockTransactions::LimitMemoryUsage` evicts oldest queued
  transactions once that queue exceeds its cap. `src/validation.cpp`
  (`DisconnectTip`, `Chainstate::MaybeUpdateMempoolForReorg`) then removes
  evicted descendants from the mempool.
- Block/undo disk usage is only hard-bounded in prune mode.
  `src/kernel/blockmanager_opts.h` defaults `prune_target` to `0`, so archival
  nodes intentionally do not cap blk/rev growth. When pruning is enabled,
  `src/node/blockstorage.cpp` (`FindFilesToPrune`, `CalculateCurrentUsage`,
  `UnlinkPrunedFiles`) sheds old block files; `src/validation.cpp`
  (`Chainstate::FlushStateToDisk`) also aborts with fatal low-disk errors
  rather than continuing into inconsistent writes.
- RPC/REST request queue growth is bounded. `src/httpserver.h` sets
  `DEFAULT_HTTP_WORKQUEUE = 64`; `src/httpserver.cpp` (`http_request_cb`,
  `InitHTTPServer`) rejects work with HTTP 503 once
  `g_threadpool_http.WorkQueueSize()` reaches `-rpcworkqueue`, and also caps
  request headers/body with `MAX_HEADERS_SIZE` and `MAX_SIZE`.

## Important code paths

- Transport and peer-slot pressure: `src/net.cpp` (`V1Transport::readHeader`,
  `CNode::MarkReceivedMsgsForProcessing`, `CNode::PollMessage`,
  `SocketSendData`, `CConnman::SocketHandlerConnected`,
  `CConnman::InactivityCheck`, `CConnman::AcceptConnection`,
  `CConnman::AttemptToEvictConnection`); `src/init.cpp`
  (`-maxconnections`, `-maxreceivebuffer`, `-maxsendbuffer`).
- Message-size and request batching limits: `src/net_processing.cpp`
  (`ProcessMessage` handling `INV`, `GETDATA`, `HEADERS`; `SendMessages`).
- Transaction download and orphan pressure:
  `src/node/txdownloadman_impl.cpp` (`AddTxAnnouncement`,
  `MaybeAddOrphanResolutionCandidate`, `GetRequestsToSend`, `ReceivedTx`);
  `src/node/txorphanage.cpp` (`AddTx`, `LimitOrphans`, `EraseForPeer`,
  `EraseForBlock`, `AddChildrenToWorkSet`).
- Mempool and reorg memory shedding: `src/validation.cpp`
  (`LimitMempoolSize`, `MemPoolAccept::AcceptSingleTransaction`,
  `MemPoolAccept::SubmitPackage`, `Chainstate::MaybeUpdateMempoolForReorg`,
  `DisconnectTip`); `src/txmempool.cpp` (`Expire`, `GetMinFee`, `TrimToSize`);
  `src/kernel/disconnected_transactions.cpp`.
- Disk pressure and pruning: `src/node/blockstorage.cpp`
  (`FindFilesToPrune`, `CalculateCurrentUsage`, `UnlinkPrunedFiles`);
  `src/validation.cpp` (`Chainstate::FlushStateToDisk`).
- HTTP queue backpressure: `src/httpserver.cpp` (`http_request_cb`,
  `InitHTTPServer`); `src/httpserver.h`.

## Related tests

- `test/functional/p2p_invalid_messages.py`
- `test/functional/p2p_timeouts.py`
- `test/functional/p2p_tx_download.py`
- `test/functional/p2p_orphan_handling.py`
- `src/test/txdownload_tests.cpp`
- `src/test/orphanage_tests.cpp`
- `test/functional/mempool_limit.py`
- `test/functional/rpc_packages.py`
- `test/functional/feature_pruning.py`
- `test/functional/interface_rpc.py`

## Adjacent pages

- `[[areas/p2p-and-networking]]`
- `[[areas/mempool-and-policy]]`
- `[[areas/validation-and-chainstate]]`
- `[[areas/rpc-rest-zmq-and-interfaces]]`
- `[[files/src/init.cpp]]`
- `[[files/src/net.cpp]]`
- `[[files/src/net_processing.cpp]]`
- `[[files/src/node/txorphanage.cpp]]`
- `[[files/src/txmempool.cpp]]`
- `[[files/src/validation.cpp]]`
- `[[workflows/transaction-acceptance]]`
- `[[workflows/block-validation-and-connection]]`
- `[[investigations/critical-test-coverage-gaps]]`

## Open questions

- `src/node/txdownloadman_impl.cpp` (`MaybeAddOrphanResolutionCandidate`) has
  an explicit TODO to add orphan-resolution-specific delays and limits,
  instead of only mirroring the normal `m_txrequest` announcement limits.
- The consulted validation paths do not expose a hard wall-clock CPU budget for
  consensus-valid block or transaction script verification. Current mitigation
  is mostly work ordering (`src/validation.cpp`
  (`MemPoolAccept::AcceptSingleTransaction`) runs inexpensive checks before
  script verification), caching (`src/validation.h` / `src/validation.cpp`
  (`ValidationCache`)), and parallelization (`src/validation.h`
  (`ChainstateManager::m_script_check_queue`)), not explicit CPU shedding.
- Archival block storage remains intentionally unbounded when
  `kernel::BlockManagerOpts::prune_target == 0`; operators that disable
  pruning are relying on external disk provisioning rather than on an
  in-process cap.

## Sources consulted

- `src/init.cpp`
- `src/net.h`
- `src/net.cpp`
- `src/net_processing.h`
- `src/net_processing.cpp`
- `src/node/txdownloadman.h`
- `src/node/txdownloadman_impl.cpp`
- `src/node/txorphanage.h`
- `src/node/txorphanage.cpp`
- `src/kernel/mempool_options.h`
- `src/txmempool.cpp`
- `src/kernel/disconnected_transactions.h`
- `src/kernel/disconnected_transactions.cpp`
- `src/kernel/blockmanager_opts.h`
- `src/node/blockstorage.cpp`
- `src/validation.h`
- `src/validation.cpp`
- `src/httpserver.h`
- `src/httpserver.cpp`
- `src/serialize.h`
- `src/flatfile.cpp`
- `test/functional/p2p_invalid_messages.py`
- `test/functional/p2p_timeouts.py`
- `test/functional/p2p_tx_download.py`
- `test/functional/p2p_orphan_handling.py`
- `test/functional/mempool_limit.py`
- `test/functional/feature_pruning.py`
- `test/functional/interface_rpc.py`
- `test/functional/rpc_packages.py`
- `src/test/net_tests.cpp`
- `src/test/net_peer_connection_tests.cpp`
- `src/test/txdownload_tests.cpp`
- `src/test/orphanage_tests.cpp`
