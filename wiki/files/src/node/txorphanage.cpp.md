---
kind: file
title: src/node/txorphanage.cpp
status: active
last_reviewed: 2026-04-21
paths:
  - src/node/txorphanage.cpp
tags:
  - p2p
  - orphanage
  - dos
  - relay
---

# src/node/txorphanage.cpp

## Role in the System

`src/node/txorphanage.cpp` implements `node::TxOrphanageImpl`, the bounded store
for transactions that failed mempool admission with
`TxValidationResult::TX_MISSING_INPUTS`. The file is part of the transaction
download/orphan-resolution path owned by `node::TxDownloadManagerImpl`, and it
is designed around attacker pressure rather than convenience: it stores orphan
transactions, attributes them to announcer peers, exposes parent-to-child
lookups, and enforces memory and latency-style limits before the orphanage can
grow without bound (`src/node/txorphanage.cpp`
`node::TxOrphanageImpl::AddTx`, `LimitOrphans`; `src/node/txorphanage.h`
`node::TxOrphanage`).

The class is explicitly not thread-safe. Callers must provide external
synchronization (`src/node/txorphanage.h` `node::TxOrphanage`), and the current
call path runs through `node::TxDownloadManagerImpl` and then
`PeerManagerImpl::ProcessOrphanTx` in `src/net_processing.cpp`.

## Important Types and Functions

- `node::TxOrphanageImpl::Announcement` is the atomic stored item: one
  `{wtxid, peer}` announcement with a shared `CTransactionRef`, the announcing
  `NodeId`, a monotonic `m_entry_sequence`, and a `m_reconsider` flag used as a
  per-peer work-set marker. The implementation permits multiple announcers for
  one `wtxid`, and also permits multiple transactions with the same `txid` but
  different witnesses because the primary identity is `GetWitnessHash()`
  (`src/node/txorphanage.cpp` `Announcement`, `ByWtxid`, `AddTx`, `HaveTx`).
- `Announcement::GetMemUsage()` approximates orphanage memory pressure with
  `GetTransactionWeight(*m_tx)`. `Announcement::GetLatencyScore()` approximates
  expensive paths with `1 + (vin.size() / 10)`, so wide fan-in transactions are
  treated as higher-cost even if they are not especially large by weight
  (`src/node/txorphanage.cpp` `Announcement::GetMemUsage`,
  `Announcement::GetLatencyScore`).
- `m_orphans` is a `boost::multi_index_container` with two ordered indices:
  `ByWtxid` on `{wtxid, peer}` and `ByPeer` on `{peer, reconsider, sequence}`.
  The `ByPeer` sort order is security-relevant: within a peer, the oldest
  non-reconsiderable announcement is encountered first, so trimming sacrifices
  stale non-work items before reconsiderable ones
  (`src/node/txorphanage.cpp` `OrphanIndices`, `ByPeerViewExtractor`,
  `LimitOrphans`).
- `m_outpoint_to_orphan_wtxids` is the parent lookup table:
  `std::unordered_map<COutPoint, std::set<Wtxid>, SaltedOutpointHasher>`. It is
  populated only once per unique `wtxid`, not once per announcer, and it is the
  index used for both parent-accepted wakeups and block-conflict erasure
  (`src/node/txorphanage.cpp` `m_outpoint_to_orphan_wtxids`, `AddTx`,
  `Erase`, `AddChildrenToWorkSet`, `EraseForBlock`).
- `m_peer_orphanage_info` stores `PeerDoSInfo` per active announcer peer. Its
  counters are not deduplicated across announcers: one shared orphan counts once
  in `TotalOrphanUsage()` but once per announcing peer in `UsageByPeer()` and
  `LatencyScoreFromPeer()` (`src/node/txorphanage.cpp` `PeerDoSInfo`,
  `UsageByPeer`, `LatencyScoreFromPeer`, `TotalOrphanUsage`).
- `AddTx()` is the main insertion path. It rejects transactions above
  `MAX_STANDARD_TX_WEIGHT` to avoid large-orphan memory abuse, inserts the new
  announcement, updates `m_peer_orphanage_info`, and only on the first
  announcer updates deduplicated usage/latency counters and the outpoint map.
  It always ends with `LimitOrphans()`
  (`src/node/txorphanage.cpp` `AddTx`).
- `AddTx()` returns `brand_new`, not “whether any state changed”. If the same
  `wtxid` arrives from a different peer, insertion succeeds, peer accounting is
  updated, and the return value is still `false` because the orphan was already
  present (`src/node/txorphanage.cpp` `AddTx`). Reviewers should not interpret a
  `false` return as a no-op.
- `AddAnnouncer()` is the explicit “new peer, existing orphan” path. It copies
  the stored `CTransactionRef`, inserts a new `{wtxid, peer}` announcement,
  updates per-peer accounting, and trims if needed, but it does not touch the
  deduplicated usage or outpoint indexes because those are keyed by unique
  `wtxid` (`src/node/txorphanage.cpp` `AddAnnouncer`).
- `LimitOrphans()` is the core backpressure mechanism. `NeedsTrim()` fires when
  `TotalLatencyScore() > MaxGlobalLatencyScore()` or
  `TotalOrphanUsage() > MaxGlobalUsage()`. The trim logic computes a per-peer
  DoS score with `PeerDoSInfo::GetDosScore(max_lat, max_mem)`, keeps only peers
  with score `> 1`, and repeatedly evicts the current worst peer’s oldest
  announcement until the global limits hold again. This is the code mechanism
  behind the “reserved share per peer” behavior described in
  `src/node/txorphanage.h`: trimming walks the current worst peer’s own
  `ByPeer` range instead of evicting a global oldest announcement
  (`src/node/txorphanage.cpp` `NeedsTrim`, `PeerDoSInfo::GetDosScore`,
  `LimitOrphans`).
- The two resource budgets are intentionally asymmetric. `MaxGlobalLatencyScore`
  is fixed at construction and defaults to
  `DEFAULT_MAX_ORPHANAGE_LATENCY_SCORE` (3000), while `MaxGlobalUsage()` scales
  with the number of active peers as
  `m_reserved_usage_per_peer * max(m_peer_orphanage_info.size(), 1)` and
  defaults to 404,000 weight units per active peer. In contrast,
  `MaxPeerLatencyScore()` shrinks as more peers become active because the fixed
  global latency budget is divided across them
  (`src/node/txorphanage.h`
  `DEFAULT_MAX_ORPHANAGE_LATENCY_SCORE`,
  `DEFAULT_RESERVED_ORPHAN_WEIGHT_PER_PEER`; `src/node/txorphanage.cpp`
  `MaxGlobalLatencyScore`, `ReservedPeerUsage`, `MaxGlobalUsage`,
  `MaxPeerLatencyScore`).
- `Erase()` is the common deletion helper. It updates peer counters, removes the
  peer entry entirely once empty, and only if the erased announcement was the
  last one for that `wtxid` does it decrement `m_unique_orphans`,
  `m_unique_orphan_usage`, `m_unique_rounded_input_scores`, and the parent
  outpoint map. The helper also clears `m_reconsiderable_wtxids` when the
  erased announcement carried `m_reconsider=true`
  (`src/node/txorphanage.cpp` `Erase`, `IsUnique`).
- `EraseTxInternal()` removes all announcers for one `wtxid`. The public
  `EraseTx()` wrapper then calls `LimitOrphans()` again because removing peers
  can lower `MaxGlobalUsage()` by shrinking the active-peer count
  (`src/node/txorphanage.cpp` `EraseTxInternal`, `EraseTx`,
  `MaxGlobalUsage`).
- `EraseForPeer()` removes every announcement attributed to one peer, but
  orphans announced by other peers survive. This is the peer-attribution cleanup
  path used on disconnect, and it also retrims afterward because active-peer
  count may have decreased (`src/node/txorphanage.cpp` `EraseForPeer`).
- `AddChildrenToWorkSet()` is the “parent accepted” wakeup path. For each output
  of the accepted parent, it looks up dependent orphan `wtxid`s in
  `m_outpoint_to_orphan_wtxids`, skips any `wtxid` that already has a
  reconsiderable announcement, chooses one current announcer at random, marks
  exactly that announcement `m_reconsider=true`, and records the `wtxid` in
  `m_reconsiderable_wtxids` (`src/node/txorphanage.cpp`
  `AddChildrenToWorkSet`).
- `GetTxToReconsider()` drains one peer’s work set by looking for the first
  `ByPeer` entry with `{peer, true, 0}`, flipping its `m_reconsider` flag back
  to `false`, removing the `wtxid` from `m_reconsiderable_wtxids`, and returning
  the transaction reference. The orphan can remain stored after reconsideration;
  “ready for work” and “present in orphanage” are separate states
  (`src/node/txorphanage.cpp` `GetTxToReconsider`, `HaveTxToReconsider`).
- `EraseForBlock()` batches deletion of all orphan `wtxid`s whose inputs conflict
  with inputs spent by transactions in the connected block. Because the lookup is
  driven by `m_outpoint_to_orphan_wtxids`, it removes direct conflicts and
  “same txid, different witness” conflicts just by matching spent prevouts
  (`src/node/txorphanage.cpp` `EraseForBlock`).
- `GetChildrenFromSamePeer()` reverse-scans one peer’s range in `ByPeer` order
  and returns newest-first children spending outputs of a given parent. That
  ordering is consumed by opportunistic 1p1c package selection in
  `node::TxDownloadManagerImpl::Find1P1CPackage`
  (`src/node/txorphanage.cpp` `GetChildrenFromSamePeer`; `src/node/txdownloadman_impl.cpp`
  `Find1P1CPackage`).
- `SanityCheck()` reconstructs peer accounting, reconsiderable state, the
  outpoint map, deduplicated usage, deduplicated latency score, and finally
  asserts `!NeedsTrim()`. The implementation intends every public mutation path
  to leave the orphanage back under both resource limits
  (`src/node/txorphanage.cpp` `SanityCheck`, `NeedsTrim`).
- Expiration is not time-based in this file. `TxOrphanageImpl` stores no wall
  clock timestamps; `m_entry_sequence` exists only to define insertion order for
  eviction and peer-local child ordering. In current code, an orphan leaves this
  file only through trimming, explicit erase calls, connected-block conflict
  handling, or caller decisions after acceptance/rejection in
  `TxDownloadManagerImpl` (`src/node/txorphanage.cpp` `m_current_sequence`,
  `LimitOrphans`, `EraseTx`, `EraseForPeer`, `EraseForBlock`; `src/node/txdownloadman_impl.cpp`
  `MempoolAcceptedTx`, `MempoolRejectedTx`).

## Callers and Dependencies

- `src/node/txdownloadman_impl.cpp` is the main caller:
  `TxDownloadManagerImpl::MempoolRejectedTx` stores missing-input transactions
  through `AddTx()`, `AddTxAnnouncement` can add later announcers through
  `AddAnnouncer()`, `MempoolAcceptedTx` calls `AddChildrenToWorkSet()` and then
  `EraseTx()`, `DisconnectedPeer` calls `EraseForPeer()`, `BlockConnected` calls
  `EraseForBlock()`, and `HaveMoreWork`/`GetTxToReconsider` expose per-peer
  reconsideration work.
- `src/net_processing.cpp` consumes the reconsideration interface.
  `PeerManagerImpl::ProcessOrphanTx` loops on `GetTxToReconsider(peer)` and runs
  `m_chainman.ProcessTransaction()` on returned orphans, which is the current
  “do some orphan work after parent acceptance” path rather than recursively
  walking descendants in `txorphanage.cpp` itself.
- `policy/policy.h` supplies `MAX_STANDARD_TX_WEIGHT`, which is the hard reject
  threshold for oversized orphan entries (`src/node/txorphanage.cpp` `AddTx`).
- `util/feefrac.h` supplies `FeeFrac`, used to compare per-peer latency and
  memory ratios without lossy floating-point arithmetic in `PeerDoSInfo`.
- `SaltedOutpointHasher` is applied to the attacker-controlled `COutPoint` keys
  in `m_outpoint_to_orphan_wtxids`, which is the map used by the most
  input-driven orphanage paths.
- `boost::multi_index_container` is the storage backbone. The chosen indices
  enforce uniqueness for exact `{wtxid, peer}` announcements while still
  supporting fast lookup by orphan identity and per-peer eviction order.
- Time-based request expiry lives outside this file. Parent-request timeout and
  retry are handled by `TxDownloadManagerImpl::GetRequestsToSend()` and
  `TxRequestTracker`, not by `TxOrphanageImpl` itself
  (`src/node/txdownloadman_impl.cpp` `GetRequestsToSend`).

## Related Tests

- `src/test/orphanage_tests.cpp`
  `peer_dos_limits` is the main unit test for memory/latency limits, FIFO
  eviction, “non-reconsiderable before reconsiderable” trimming, multiple-peer
  victim selection, dynamic limit changes as peers appear/disappear, large-tx
  eviction bursts, and latency accounting by input count.
- `src/test/orphanage_tests.cpp`
  `too_large_orphan_tx` checks the `MAX_STANDARD_TX_WEIGHT` gate in `AddTx()`.
- `src/test/orphanage_tests.cpp`
  `process_block` checks `EraseForBlock()` against included transactions,
  conflicting transactions, partial-input conflicts, and same-`txid`
  different-witness cases.
- `src/test/orphanage_tests.cpp`
  `multiple_announcers` checks `AddAnnouncer()`, deduplicated unique-orphan
  accounting, peer-specific erasure, and the rule that block erasure removes the
  orphan for all announcers.
- `src/test/orphanage_tests.cpp`
  `same_txid_diff_witness`, `get_children`, and `peer_worksets` cover
  wtxid-based identity, peer-scoped child selection, random announcer
  assignment for reconsideration, and cleanup of reconsiderable entries when
  the assigned peer is erased.
- `src/test/orphanage_tests.cpp`
  `DoS_mapOrphans` is older but still useful coverage for mass orphan insertion,
  rejection of very large orphans, and `EraseForPeer()` behavior.
- `src/bench/txorphanage.cpp`
  `OrphanageSinglePeerEviction`, `OrphanageMultiPeerEviction`,
  `OrphanageEraseForBlock`, and `OrphanageEraseForPeer` are targeted
  microbenchmarks for worst-case trim and deletion workloads.
- `src/test/fuzz/txorphan.cpp`
  `txorphan` fuzzes all public orphanage operations and checks invariant
  relationships such as `HaveTx()` versus `GetTx()`, usage monotonicity, and
  consistency after `EraseForBlock()` and `EraseForPeer()`.
- `src/test/fuzz/txorphan.cpp`
  `txorphan_protected` specifically tests the design claim that peers which stay
  under their personal weight and latency budgets do not lose their own orphans
  during other peers’ abuse.
- `src/test/fuzz/txorphan.cpp`
  `txorphanage_sim` compares the real orphanage against a simpler simulation
  across random topologies, announcer sets, and reconsideration events.
- `src/test/txdownload_tests.cpp` and `src/test/fuzz/txdownloadman.cpp`
  exercise the caller side: when missing-input transactions enter the orphanage,
  when connected blocks remove them, and how reconsideration work interacts with
  request tracking.
- `test/functional/p2p_orphan_handling.py`
  covers RPC-visible orphan attribution through `getorphantxs`, preferred-peer
  parent fetching, retry after request timeout, announcer shrinkage on
  disconnect, changing missing-parent sets, and protection of a maximally sized
  honest package under adversarial pressure.
- `test/functional/p2p_opportunistic_1p1c.py`
  `test_orphanage_dos_large` and `test_orphanage_dos_many` check that orphan
  resolution still succeeds when attackers occupy large parts of orphanage space
  or announcement budget.

## Notes or Risks

- Verified: there is no wall-clock TTL in `TxOrphanageImpl`. Old age only matters
  for eviction order through `m_entry_sequence`. Stale orphans persist until a
  block conflicts with them, their peer disconnects, a caller explicitly erases
  them, or resource trimming removes them.
- Verified: global and per-peer accounting answer different questions.
  `TotalOrphanUsage()` and `TotalLatencyScore()` are deduplicated by `wtxid`,
  while `UsageByPeer()` and `LatencyScoreFromPeer()` charge each announcer.
  Reviewing attacker amplification requires looking at both views.
- Verified: active-peer count is itself part of the resource model.
  When a new peer becomes the first announcer of any orphan, `MaxGlobalUsage()`
  increases, but `MaxPeerLatencyScore()` for every active peer decreases because
  the fixed global latency budget is redivided.
- Verified: trim victim selection is peer-local, not global FIFO.
  `LimitOrphans()` first identifies the current worst peer by DoS score and then
  erases from that peer’s own `ByPeer` range, oldest non-reconsiderable first.
- Verified: reconsideration state is single-assignment per `wtxid`.
  If the one announcement carrying `m_reconsider=true` is erased, `Erase()`
  removes that `wtxid` from `m_reconsiderable_wtxids`; this file does not
  automatically promote another announcer.
- Review focus: `EraseForBlock()` and `LimitOrphans()` are the expensive paths
  this file is trying to bound. The latency-score heuristic is only an
  approximation, so performance-sensitive changes should be checked against the
  existing bench targets and the input-heavy unit cases, not just functional
  correctness.
- Review focus: storing same-`txid`, different-witness transactions is
  intentional because identity is keyed by `wtxid`. Changes that collapse the
  orphanage onto `txid` semantics would interact with witness-malleation relay
  behavior, not just memory usage.

## Sources Consulted

- `src/node/txorphanage.cpp`
- `src/node/txorphanage.h`
- `src/node/txdownloadman_impl.cpp`
- `src/node/txdownloadman_impl.h`
- `src/net_processing.cpp`
- `src/test/orphanage_tests.cpp`
- `src/bench/txorphanage.cpp`
- `src/test/fuzz/txorphan.cpp`
- `src/test/fuzz/txdownloadman.cpp`
- `src/test/txdownload_tests.cpp`
- `test/functional/p2p_orphan_handling.py`
- `test/functional/p2p_opportunistic_1p1c.py`
