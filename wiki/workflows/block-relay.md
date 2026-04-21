---
kind: workflow
title: Block Relay
status: active
last_reviewed: 2026-04-21
paths:
  - src/net_processing.cpp
  - src/net_processing.h
  - src/validation.cpp
tags:
  - block-relay
  - headers
  - compact-blocks
  - p2p
---

# Block Relay

## Summary

Block relay is split between networking state in `src/net_processing.cpp`
(`PeerManagerImpl`) and validation entry points in `src/validation.cpp`
(`ChainstateManager`). `PeerManagerImpl` negotiates how peers announce blocks,
tracks which headers and blocks each peer is believed to have, decides when to
request `headers`, `cmpctblock`, `blocktxn`, or full `block` data, and serves
blocks back to peers. Validation begins once headers are handed to
`ChainstateManager::ProcessNewBlockHeaders()` or full or reconstructed blocks
are handed to `PeerManagerImpl::ProcessBlock()`, which calls
`ChainstateManager::ProcessNewBlock()`.

## Workflow

### 1. Negotiate announcement style

- `src/net_processing.cpp` (`PeerManagerImpl::ProcessMessage`) records
  `SENDHEADERS` by setting `peer.m_prefers_headers`.
- The same dispatcher records `SENDCMPCT` version 2 by setting
  `CNodeState::m_provides_cmpctblocks` and
  `CNodeState::m_requested_hb_cmpctblocks`.
- `src/net_processing.cpp` (`PeerManagerImpl::MaybeSendSendHeaders`) delays our
  own `SENDHEADERS` until the peer has demonstrated a best-known block above
  `MinimumChainWork()`. The source comment calls out that this avoids mixing
  block announcements into initial headers sync state.
- `src/net_processing.cpp`
  (`PeerManagerImpl::MaybeSetPeerAsAnnouncingHeaderAndIDs`) upgrades at most
  three peers into BIP152 high-bandwidth announcers, tries not to evict the
  last outbound high-bandwidth peer, and returns early in
  `m_opts.ignore_incoming_txs` mode (`-blocksonly`) because compact block
  reconstruction would not have a useful mempool.

### 2. Handle inbound announcements

- `INV` path:
  `src/net_processing.cpp` (`PeerManagerImpl::ProcessMessage`) updates peer
  block availability through `UpdateBlockAvailability()`. If the announced
  block is new and not already in flight, it may call `MaybeSendGetHeaders()`
  starting from `m_chainman.m_best_header`.
- During initial headers sync, that `INV`-triggered `GETHEADERS` path is
  deliberately throttled by `peer.m_inv_triggered_getheaders_before_sync` and
  `m_last_block_inv_triggering_headers_sync`, so only one extra peer per newly
  announced block is recruited for headers sync.
- `HEADERS` path:
  `src/net_processing.cpp` (`PeerManagerImpl::ProcessHeadersMessage`) first
  runs `CheckHeadersPoW()` to require valid proof of work and a continuous
  header chain before any deeper processing.
- If the first header does not connect to the local block index,
  `ProcessHeadersMessage()` calls `HandleUnconnectingHeaders()`, which sends a
  `GETHEADERS` from the current best header and stores the peer's last unknown
  hash through `UpdateBlockAvailability()` so the peer can still become a
  download source later.
- Low-work headers are not stored eagerly. `ProcessHeadersMessage()` uses
  `GetAntiDoSWorkThreshold()` plus `TryLowWorkHeadersSync()` to route such
  chains through `HeadersSyncState` first. Only peers with
  `NetPermissionFlags::NoBan` bypass that anti-DoS gate.
- Successful header batches are handed to
  `src/validation.cpp` (`ChainstateManager::ProcessNewBlockHeaders`), which
  loops through `AcceptBlockHeader()` and
  `ContextualCheckBlockHeader()`.
- After accepted headers, `src/net_processing.cpp`
  (`PeerManagerImpl::UpdatePeerStateForReceivedHeaders`) updates the peer's
  best-known block and headers-sync state, then
  `HeadersDirectFetchBlocks()` may immediately request blocks toward the new
  header tip.
- Near tip, `HeadersDirectFetchBlocks()` can upgrade a single-block request
  from `MSG_BLOCK | GetFetchFlags(peer)` to `MSG_CMPCT_BLOCK` when compact
  relay is usable for that peer.
- `CMPCTBLOCK` path:
  `src/net_processing.cpp` (`PeerManagerImpl::ProcessMessage`) first checks
  that the previous header is known and that the announced header clears
  `GetAntiDoSWorkThreshold()`. The header itself is then processed through
  `ProcessNewBlockHeaders()`.
- If the compact block is close enough to the tip, `ProcessMessage()` creates
  or reuses a `PartiallyDownloadedBlock`, seeds it from the mempool and
  `vExtraTxnForCompact`, and then either reconstructs immediately, sends
  `GETBLOCKTXN`, falls back to full-block `GETDATA`, or degrades to plain
  header processing for older or far-ahead announcements.
- `BLOCKTXN` path:
  `src/net_processing.cpp` (`PeerManagerImpl::ProcessCompactBlockTxns`) fills
  the stored `PartiallyDownloadedBlock`. A successful fill hands a full block
  to `ProcessBlock(..., force_processing=true, min_pow_checked=true)`. Failed
  fills either fall back to full-block download or treat repeated responses as
  misbehavior.
- `BLOCK` path:
  `src/net_processing.cpp` (`PeerManagerImpl::ProcessMessage`) removes the
  in-flight request for that peer, records the source in `mapBlockSource`,
  derives `min_pow_checked` from `GetAntiDoSWorkThreshold()`, and passes the
  block to `ProcessBlock()`.

### 3. Schedule downloads and track in-flight work

- `src/net_processing.cpp` (`PeerManagerImpl::SendMessages`) is the steady
  block download scheduler. If a peer can serve blocks and is eligible for
  sync, it calls `FindNextBlocksToDownload()` as long as the peer has fewer
  than `MAX_BLOCKS_IN_TRANSIT_PER_PEER` blocks in flight.
- `src/net_processing.cpp`
  (`PeerManagerImpl::FindNextBlocksToDownload`, `FindNextBlocks`) walks from
  the peer's last common block toward its best-known block, bounded by
  `BLOCK_DOWNLOAD_WINDOW`.
- That walk skips blocks already on disk, already in flight, headers on invalid
  subtrees, post-SegWit blocks from non-witness peers, and blocks that a
  `NODE_NETWORK_LIMITED` peer cannot serve safely.
- `src/net_processing.cpp`
  (`PeerManagerImpl::BlockRequested`, `RemoveBlockRequest`) keeps
  `mapBlocksInFlight`, per-peer `vBlocksInFlight`, `m_downloading_since`, and
  stalling timers consistent across `headers`, `cmpctblock`, and `block`
  driven downloads.
- `SendMessages()` starts `m_stalling_since` when the download window is
  blocked and disconnects peers that continue stalling past the adaptive block
  download timeout.

### 4. Hand block data to validation

- `src/net_processing.cpp` (`PeerManagerImpl::ProcessBlock`) is the networking
  to validation handoff. It calls
  `src/validation.cpp` (`ChainstateManager::ProcessNewBlock`).
- `src/validation.cpp` (`ChainstateManager::ProcessNewBlock`) locks
  `::cs_main`, runs `CheckBlock()`, and only then calls `AcceptBlock()`. The
  source comment explains that `CheckBlock()` failures are intentionally not
  cached as block invalidity, to avoid hard-failing on unknown malleation
  classes.
- `src/validation.cpp` (`ChainstateManager::AcceptBlock`) calls
  `AcceptBlockHeader()`, rejects many unrequested blocks unless they are close
  enough, high-work, and otherwise eligible, then runs `CheckBlock()` plus
  `ContextualCheckBlock()`.
- If a new block extends `ActiveTip()` and the node is not in IBD,
  `AcceptBlock()` emits `NewPoWValidBlock()` before activation. This is the
  fast-relay point for tip-extending valid blocks.
- `AcceptBlock()` then writes the block to disk and records transaction
  positions through `ReceivedBlockTransactions()`. Activation onto the active
  chain happens later through the normal best-chain path documented in
  `[[workflows/block-validation-and-connection]]`.
- `src/net_processing.cpp`
  (`PeerManagerImpl::BlockChecked`, `MaybePunishNodeForBlock`) handles the
  post-validation relay consequences. Invalid headers and invalid full blocks
  can discourage peers, while compact-block relay is treated more cautiously:
  BIP152 allows a peer to announce a header before full block validation, so a
  compact-block source is not automatically discouraged just because the later
  block body fails consensus checks.

### 5. Relay validated blocks outward

- `src/net_processing.cpp` (`PeerManagerImpl::UpdatedBlockTip`) queues up to
  `MAX_BLOCKS_TO_ANNOUNCE` best-chain hashes per peer in
  `peer.m_blocks_for_headers_relay`. It does not queue block announcements
  during initial block download.
- `src/net_processing.cpp` (`PeerManagerImpl::NewPoWValidBlock`) caches the
  most recent full block and compact block, then fast-announces a single
  `CMPCTBLOCK` to high-bandwidth peers that are believed to have the previous
  header but not this one. The function returns early before SegWit activation.
- `src/net_processing.cpp` (`PeerManagerImpl::SendMessages`) chooses the
  outbound announcement type per peer:
  - send `CMPCTBLOCK` when there is one new block and the peer requested
    high-bandwidth compact relay
  - else send `HEADERS` if the peer sent `SENDHEADERS` and the queued headers
    connect from something the peer is believed to have
  - else fall back to `INV`, usually only for the current tip
- The fallback logic is intentionally conservative. If queued headers do not
  connect cleanly from the peer's known chain, `SendMessages()` abandons the
  header path and reverts to `INV` instead of guessing.

### 6. Serve blocks back to peers

- `src/net_processing.cpp` (`PeerManagerImpl::ProcessGetBlockData`) serves
  `BLOCK`, `CMPCTBLOCK`, or `MERKLEBLOCK` plus matching `TX`s, depending on the
  requested inventory type.
- Old or side-chain blocks are only served when
  `src/net_processing.cpp` (`PeerManagerImpl::BlockRequestAllowed`) says they
  are either on the active chain or are recent-enough valid stale-relay
  candidates under `STALE_RELAY_AGE_LIMIT`.
- `ProcessGetBlockData()` disconnects peers requesting historical blocks once
  `CConnman::OutboundTargetReached(true)` and the `-maxuploadtarget` budget no
  longer allows historical serving, unless the peer has
  `NetPermissionFlags::Download`.
- The same function disconnects peers who ask a `NODE_NETWORK_LIMITED` node for
  blocks older than the 288-block window plus the 2-block race buffer. The
  source comment states that this avoids leaking the prune height; peers with
  `NetPermissionFlags::NoBan` bypass that check.
- Compact responses are also intentionally bounded:
  `MAX_CMPCTBLOCK_DEPTH` limits how deep a requested `CMPCTBLOCK` can be before
  the node falls back to a full block, and `MAX_BLOCKTXN_DEPTH` limits how deep
  `GETBLOCKTXN` can reach before the node enqueues a full-block response
  instead.

## Networking vs Validation vs Relay Policy

- Networking responsibilities live in `src/net_processing.cpp`
  (`PeerManagerImpl`): peer capability negotiation, what each peer is assumed
  to know, in-flight download queues, announcement choice, compact block
  reconstruction bookkeeping, and block serving.
- Validation responsibilities live in `src/validation.cpp`
  (`ChainstateManager::ProcessNewBlockHeaders`, `AcceptBlockHeader`,
  `ContextualCheckBlockHeader`, `ProcessNewBlock`, `AcceptBlock`,
  `ContextualCheckBlock`). These functions decide whether a header or block is
  actually valid and whether it becomes eligible for activation.
- Relay policy in this workflow is separate from consensus. Examples include
  `GetAntiDoSWorkThreshold()`, `MinimumChainWork()`, `-blocksonly` disabling
  compact fetches and high-bandwidth selection, `NODE_NETWORK_LIMITED`,
  `STALE_RELAY_AGE_LIMIT`, and `-maxuploadtarget`. Failing one of those checks
  does not mean a block is consensus-invalid.
- Mempool policy is almost absent from block relay. The main exception is that
  compact block reconstruction uses mempool contents opportunistically, but
  whether a block is accepted is still decided by block validation, not by
  transaction relay policy.

## Resource and Privacy-Sensitive Behavior

- Low-work header chains are screened before permanent storage by
  `CheckHeadersPoW()`, `GetAntiDoSWorkThreshold()`, and `TryLowWorkHeadersSync()`.
- `MaybeSendGetHeaders()` keeps only one recent `GETHEADERS` outstanding per
  peer by enforcing `HEADERS_RESPONSE_TIME`, which limits redundant
  request-response churn during announcement storms.
- Block download has explicit concurrency and window caps:
  `MAX_BLOCKS_IN_TRANSIT_PER_PEER`, `BLOCK_DOWNLOAD_WINDOW`, and
  `MAX_CMPCTBLOCKS_INFLIGHT_PER_BLOCK` in `src/net_processing.h`.
- Compact block reconstruction is deliberately conservative: repeated
  contradictory `BLOCKTXN` responses trigger fallback or misbehavior handling in
  `ProcessCompactBlockTxns()`.
- `ProcessGetBlockData()` protects availability by refusing old-block service
  once the historical upload budget is exhausted, and by refusing deep compact
  or `GETBLOCKTXN` responses that would turn disk reads into cheap remote work.
- `ProcessGetBlockData()` also protects operator privacy for pruned nodes by
  disconnecting peers that probe below the `NODE_NETWORK_LIMITED` service
  window, rather than revealing exactly how far back block data is still
  available.

## Related Tests

- `test/functional/p2p_sendheaders.py` covers `SENDHEADERS`, headers-vs-inv
  announcements, direct fetch after headers, large-reorg fallback to `INV`, and
  repeated unconnecting-header recovery.
- `test/functional/p2p_compactblocks.py` covers `SENDCMPCT` negotiation,
  compact block construction, `GETBLOCKTXN` round trips, low-work compact
  blocks, old compact blocks, invalid and repeated `BLOCKTXN` responses, and
  end-to-end compact block announcements.
- `test/functional/p2p_compactblocks_blocksonly.py` covers the
  `-blocksonly` boundary: no high-bandwidth selection and no compact fetch
  requests, but compact block serving still works.
- `test/functional/p2p_compactblocks_hb.py` covers high-bandwidth peer
  selection, including the reserved outbound slot.
- `test/functional/p2p_initial_headers_sync.py` covers one-peer-at-a-time
  initial headers sync, one-extra-peer-per-new-block recruitment, and headers
  timeout or disconnect behavior.
- `test/functional/p2p_headers_sync_with_minchainwork.py` and
  `test/functional/feature_minchainwork.py` cover suppression of low-work
  headers and block relay before `-minimumchainwork` is reached.
- `test/functional/p2p_unrequested_blocks.py` covers acceptance boundaries for
  unrequested blocks and the `INV`-triggered recovery of missing intermediate
  blocks.
- `test/functional/p2p_mutated_blocks.py` covers the mutated-block defense that
  prevents an attacker from clearing another peer's in-flight compact-block
  state.
- `test/functional/p2p_node_network_limited.py` and
  `test/functional/feature_maxuploadtarget.py` cover serving-side limits for
  pruned or bandwidth-limited nodes.
- `test/functional/p2p_segwit.py` covers witness block request types and the
  refusal to fetch blocks from non-witness peers once SegWit is relevant.
- `src/test/headers_sync_chainwork_tests.cpp` covers the low-work
  `HeadersSyncState` presync and redownload path.
- `src/test/blockencodings_tests.cpp` covers `PartiallyDownloadedBlock`
  reconstruction semantics used by compact block relay.
- `src/test/peerman_tests.cpp` covers service-flag desirability around
  `NODE_NETWORK_LIMITED` peers and stale-tip proximity.

## Adjacent Pages

- `[[areas/p2p-and-networking]]`
- `[[areas/validation-and-chainstate]]`
- `[[concepts/resource-exhaustion-and-backpressure]]`
- `[[concepts/operator-privacy]]`
- `[[files/src/net_processing.cpp]]`
- `[[files/src/validation.cpp]]`
- `[[workflows/block-validation-and-connection]]`
- `[[workflows/initial-block-download]]`

## Open Questions

- `src/net_processing.cpp` currently multiplexes tip relay and
  AssumeUTXO-related historical block download in the same `SendMessages()`
  scheduler. If background validation becomes a recurring wiki topic, that
  historical path likely deserves its own workflow page instead of staying as a
  note here.

## Sources Consulted

- `src/net_processing.cpp` (`PeerManagerImpl::ProcessMessage`,
  `ProcessHeadersMessage`, `HandleUnconnectingHeaders`,
  `TryLowWorkHeadersSync`, `HeadersDirectFetchBlocks`,
  `UpdatePeerStateForReceivedHeaders`, `FindNextBlocksToDownload`,
  `FindNextBlocks`, `BlockRequested`, `RemoveBlockRequest`, `ProcessBlock`,
  `ProcessCompactBlockTxns`, `MaybeSendSendHeaders`,
  `MaybeSetPeerAsAnnouncingHeaderAndIDs`, `UpdatedBlockTip`,
  `NewPoWValidBlock`, `ProcessGetBlockData`, `BlockRequestAllowed`,
  `MaybePunishNodeForBlock`, `SendMessages`)
- `src/net_processing.h` (`MAX_CMPCTBLOCKS_INFLIGHT_PER_BLOCK`)
- `src/validation.cpp` (`ChainstateManager::ProcessNewBlockHeaders`,
  `AcceptBlockHeader`, `ContextualCheckBlockHeader`, `AcceptBlock`,
  `ProcessNewBlock`, `ReceivedBlockTransactions`)
- `test/functional/p2p_sendheaders.py`
- `test/functional/p2p_compactblocks.py`
- `test/functional/p2p_compactblocks_blocksonly.py`
- `test/functional/p2p_compactblocks_hb.py`
- `test/functional/p2p_initial_headers_sync.py`
- `test/functional/p2p_headers_sync_with_minchainwork.py`
- `test/functional/feature_minchainwork.py`
- `test/functional/p2p_unrequested_blocks.py`
- `test/functional/p2p_mutated_blocks.py`
- `test/functional/p2p_node_network_limited.py`
- `test/functional/feature_maxuploadtarget.py`
- `test/functional/p2p_segwit.py`
- `src/test/headers_sync_chainwork_tests.cpp`
- `src/test/blockencodings_tests.cpp`
- `src/test/peerman_tests.cpp`
