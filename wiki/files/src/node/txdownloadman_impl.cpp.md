---
kind: file
title: src/node/txdownloadman_impl.cpp
status: active
last_reviewed: 2026-04-21
paths:
  - src/node/txdownloadman_impl.cpp
tags:
  - p2p
  - relay
  - txrequest
  - orphanage
---

# src/node/txdownloadman_impl.cpp

## Role in the system

`src/node/txdownloadman_impl.cpp` implements the stateful half of transaction
download scheduling and orphan resolution. `node::TxDownloadManagerImpl`
combines:

- `TxRequestTracker m_txrequest` for per-peer announcement tracking, request
  timing, expiry, and retry selection
- `TxOrphanage m_orphanage` for transactions rejected with
  `TxValidationResult::TX_MISSING_INPUTS`
- rolling bloom filters for recently rejected, reconsiderable-rejected, and
  recently confirmed transactions

`PeerManagerImpl` owns the public `node::TxDownloadManager` wrapper and calls
it under `m_tx_download_mutex` while handling `inv`, `tx`, and `notfound`
messages, while sending `getdata`, while reconsidering orphans, and while
reacting to tip and peer-lifecycle events (`src/net_processing.cpp`
`PeerManagerImpl::ProcessMessage`, `SendMessages`, `ProcessOrphanTx`, and the
validation-interface callbacks near `ActiveTipChange`, `BlockConnected`, and
`BlockDisconnected`). The file is not thread-safe on its own; the external
mutex discipline is part of its contract (`src/node/txdownloadman.h`
`node::TxDownloadManager`).

## Important types and functions

- `node::TxDownloadManager` in this file is only a thin wrapper; the behavior
  lives in `node::TxDownloadManagerImpl`.
- `TxDownloadManagerImpl::AlreadyHaveTx` is the deduplication boundary. It
  checks the orphanage by wtxid only, optionally checks the reconsiderable
  reject filter, checks the recent-confirmed filter, and finally checks the
  reject filter and mempool. It intentionally does not query the orphanage by
  txid to avoid suppressing a valid transaction because a same-txid,
  different-witness orphan is already stored.
- `TxDownloadManagerImpl::ConnectedPeer` and `DisconnectedPeer` maintain
  `m_peer_info` and `m_num_wtxid_peers`, and tear down both orphanage and
  request-tracker state for departing peers.
- `TxDownloadManagerImpl::AddTxAnnouncement` is the main scheduling entry
  point. For ordinary announcements it:
  - drops already-known transactions via `AlreadyHaveTx`
  - enforces the per-peer backpressure cap from `MAX_PEER_TX_ANNOUNCEMENTS`
    for peers without relay permission
  - computes request delay from `NONPREF_PEER_TX_DELAY`,
    `TXID_RELAY_DELAY`, and `OVERLOADED_PEER_TX_DELAY`
  - records the announcement through `TxRequestTracker::ReceivedInv`
- `TxDownloadManagerImpl::AddTxAnnouncement` also special-cases an announced
  wtxid that is already in the orphanage. In that case it treats the announcer
  as a candidate source for the orphan's missing parents, and records the peer
  in `TxOrphanage::AddAnnouncer` if `MaybeAddOrphanResolutionCandidate`
  accepts it.
- `TxDownloadManagerImpl::MaybeAddOrphanResolutionCandidate` turns an orphan
  announcer into synthetic parent announcements. It mirrors the normal delay
  and dropping behavior, but its own comment notes a remaining TODO for
  orphan-resolution-specific limits beyond the shared `m_txrequest` caps.
- `TxDownloadManagerImpl::GetRequestsToSend` asks
  `TxRequestTracker::GetRequestable` for eligible downloads, logs expired
  in-flight requests, rechecks `AlreadyHaveTx` as a belt-and-suspenders guard,
  and marks selected entries requested until
  `current_time + GETDATA_TX_INTERVAL`.
- `TxDownloadManagerImpl::ReceivedNotFound` and `ReceivedTx` close the
  request-tracker loop. Both mark a peer's announcement as having answered.
  `ReceivedTx` then decides whether the transaction should be validated, should
  be dropped as already known, or should instead trigger 1-parent-1-child
  package evaluation.
- `TxDownloadManagerImpl::Find1P1CPackage` is the package builder for
  reconsiderable parents. It only considers orphan children from the same peer
  (`TxOrphanage::GetChildrenFromSamePeer`) and skips package hashes already in
  the reconsiderable filter.
- `TxDownloadManagerImpl::MempoolAcceptedTx` clears request-tracker state for
  both txid and wtxid, wakes dependent orphans through
  `TxOrphanage::AddChildrenToWorkSet`, and erases the accepted orphan entry if
  present.
- `TxDownloadManagerImpl::MempoolRejectedTx` is the main feedback path from
  mempool validation. It decides whether a failure should:
  - populate `RecentRejectsFilter`
  - populate `RecentRejectsReconsiderableFilter`
  - keep and attribute an orphan in `m_orphanage`
  - request missing parents from candidate peers
  - construct a `PackageToValidate`
- `TxDownloadManagerImpl::MempoolRejectedPackage` stores the package hash in
  the reconsiderable filter so the same failed package is not retried.
- `TxDownloadManagerImpl::ActiveTipChange`, `BlockConnected`, and
  `BlockDisconnected` update the rolling filters on chain changes. Tip changes
  clear both reject filters, block connection records both txid and wtxid in
  the recent-confirmed filter and forgets outstanding requests, and block
  disconnection clears the recent-confirmed filter to avoid relay problems
  across reorgs.

## Callers and dependencies

- `src/net_processing.cpp` is the direct caller:
  - `PeerManagerImpl::ProcessMessage` calls `AddTxAnnouncement` for transaction
    `inv`s, `ReceivedTx` for `tx` messages, and `ReceivedNotFound` for
    `notfound`
  - `PeerManagerImpl::SendMessages` calls `GetRequestsToSend` to build
    transaction `getdata`
  - `PeerManagerImpl::ProcessOrphanTx` drains `GetTxToReconsider`
  - `PeerManagerImpl`'s validation callbacks call `ActiveTipChange`,
    `BlockConnected`, `BlockDisconnected`, `MempoolAcceptedTx`,
    `MempoolRejectedTx`, and `MempoolRejectedPackage`
  - peer handshake and teardown call `ConnectedPeer` and `DisconnectedPeer`
- `TxRequestTracker` (`src/txrequest.h`) provides the scheduling invariants
  that this file relies on: one outstanding request per txhash, preferred-peer
  selection, completion/expiry tracking, and candidate-peer lookup for retry
  and orphan resolution.
- `TxOrphanage` (`src/node/txorphanage.h`) provides the memory- and
  latency-limited store of missing-input transactions, multi-announcer
  attribution, and per-peer work sets used by `HaveMoreWork` and
  `GetTxToReconsider`.
- `CTxMemPool` enters through `TxDownloadOptions::m_mempool`. The file only
  uses it for `exists()` checks; actual validation is still performed by the
  caller in `PeerManagerImpl`.
- `PackageToValidate` carries peer attribution alongside a 1-parent-1-child
  package. In the current implementation `Find1P1CPackage` constructs that
  package with the same sender for both parent and child, because the child is
  selected only from `GetChildrenFromSamePeer`.

## Related tests

- `src/test/txdownload_tests.cpp`
  - `tx_rejection_types` checks how rejection results populate the reject and
    reconsiderable filters and whether future announcements are dropped
  - `handle_missing_inputs` checks when a missing-input transaction becomes an
    orphan, how many unique parents remain requestable, and how multi-parent
    cases interact with the recency filters
- `test/functional/p2p_orphan_handling.py`
  - `test_orphan_txid_inv` covers same-txid different-witness orphan handling
    and timeout-driven parent retry
  - `test_orphan_handling_prefer_outbound` checks that preferred/outbound peers
    win parent requests first and that timeout fails over to another peer
  - `test_announcers_before_and_after` checks reuse of peers that announced the
    orphan before and after it was recognized as an orphan
  - `test_parents_change` checks that the missing-parent set is recomputed when
    a later announcer has a different view of which parents are missing
  - `test_maximal_package_protected` exercises large-orphan pressure and the
    interaction with orphanage protections
- `src/test/fuzz/txdownloadman.cpp` checks invariants between
  `TxDownloadManagerImpl`, `TxRequestTracker`, and `TxOrphanage`, including the
  per-peer announcement cap and the invariant that `GetRequestsToSend` should
  not return transactions already covered by `AlreadyHaveTx`.
- `src/test/orphanage_tests.cpp` is an important dependency test suite for the
  orphanage behaviors this file assumes, including multi-announcer retention,
  same-txid/different-witness storage, and reconsideration order.
- `src/test/txrequest_tests.cpp` is the underlying scheduler test suite for
  the timeout, preferred-peer, and retry behavior consumed here through
  `TxRequestTracker`.

## Notes or risks

- `resource/backpressure`: for peers without relay permission, ordinary
  announcements are capped at `MAX_PEER_TX_ANNOUNCEMENTS`, and peers with
  `MAX_PEER_TX_REQUEST_IN_FLIGHT` or more outstanding requests incur
  `OVERLOADED_PEER_TX_DELAY`. This bounds tracker growth, but orphan
  resolution currently shares those same controls rather than having a
  dedicated budget; `MaybeAddOrphanResolutionCandidate` calls that out as a
  TODO.
- `retry/availability`: retries are driven by
  `TxRequestTracker::GetRequestable` and `GETDATA_TX_INTERVAL`, so a silent or
  disconnected peer delays the next download attempt by one request interval
  per failure. That is a deliberate anti-duplication and anti-censorship
  tradeoff, not a guarantee of immediate failover.
- `peer attribution`: `MempoolRejectedTx` collects candidate peers for orphan
  resolution from `m_txrequest` using both the orphan's txid and, when
  applicable, wtxid. This preserves announcers seen before the node realized a
  transaction was an orphan, and later `AddTxAnnouncement` can add additional
  announcers after the orphan is already stored.
- `witness-malleation boundary`: the `AlreadyHaveTx` rule to avoid orphanage
  lookup by txid is security-relevant. Without it, a peer could block
  re-download of a real transaction by first seeding a different-witness
  orphan with the same txid.
- `package pairing`: `Find1P1CPackage` only pairs reconsiderable parents with
  children from the same peer. The comment in that function explains the goal:
  avoid repeatedly picking attacker-supplied fake children and starving a real
  child from the honest announcer.
- `privacy-sensitive behavior`: this file is primarily about correctness,
  availability, and resource control rather than wallet-origin privacy. The
  clearest relay-side privacy/integrity choice here is the extra
  `TXID_RELAY_DELAY` whenever wtxid-relay peers exist, which biases downloads
  away from txid-based relay and therefore away from witness-malleation
  ambiguity. Any privacy effect is secondary to relay correctness.

## Sources consulted

- `src/node/txdownloadman_impl.cpp`
- `src/node/txdownloadman_impl.h`
- `src/node/txdownloadman.h`
- `src/net_processing.cpp`
- `src/txrequest.h`
- `src/txrequest.cpp`
- `src/node/txorphanage.h`
- `src/test/txdownload_tests.cpp`
- `src/test/fuzz/txdownloadman.cpp`
- `src/test/orphanage_tests.cpp`
- `src/test/txrequest_tests.cpp`
- `test/functional/p2p_orphan_handling.py`
