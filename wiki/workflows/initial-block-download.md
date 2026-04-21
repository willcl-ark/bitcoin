---
kind: workflow
title: Initial Block Download
status: active
last_reviewed: 2026-04-20
paths:
  - src/validation.cpp
  - src/validation.h
  - src/chain.h
  - src/net_processing.cpp
  - doc/design/assumeutxo.md
tags:
  - ibd
  - sync
  - assumeutxo
---

# Initial Block Download

## Summary

IBD in current Bitcoin Core is a latched manager-level state, not a per-peer heuristic. `ChainstateManager::IsInitialBlockDownload()` reads `m_cached_is_ibd`, and `UpdateIBDStatus()` is the only place that can clear it. P2P sync, relay policy, and cache allocation all key off that state.

## Exit conditions and latch behavior

- `src/validation.h` documents `m_cached_is_ibd` as a lock-free flag that latches from `true` to `false`.
- `src/validation.cpp` (`ChainstateManager::UpdateIBDStatus`) will not clear IBD while blocks are still loading from disk.
- After loading finishes, exit requires `CurrentChainstate().m_chain.IsTipRecent(MinimumChainWork(), max_tip_age)`. `src/chain.h` (`CChain::IsTipRecent`) makes that concrete: the current tip must exist, have at least `MinimumChainWork()`, and have timestamp `>= now - max_tip_age`.
- Once `m_cached_is_ibd` becomes `false`, `IsInitialBlockDownload()` never returns `true` again for that process.

## Header and block sync path

1. `src/net_processing.cpp` (`ProcessHeadersMessage`) checks header PoW first, then routes low-work header chains through the headers-sync anti-DoS logic, and finally submits accepted headers to `ChainstateManager::ProcessNewBlockHeaders(..., min_pow_checked=true)`.
2. `UpdatePeerStateForReceivedHeaders()` treats IBD specially: when an outbound peer has no more headers to give and its best-known chainwork is still below `MinimumChainWork()`, the peer can be disconnected as unhelpful for sync.
3. `HeadersDirectFetchBlocks()` can immediately request blocks toward a new header tip once `CanDirectFetch()` says the node is close enough to synced.
4. In the main send loop, block download during IBD is restricted to sync peers: `CanServeBlocks(peer) && ((sync_blocks_and_headers_from_peer && !IsLimitedPeer(peer)) || !IsInitialBlockDownload())`.
5. If assumeutxo is active, block download prioritizes the current chainstate first and only then tries historical-background downloads. See `src/net_processing.cpp` (`FindNextBlocksToDownload`, `TryDownloadingHistoricalBlocks`).

## IBD-specific behavior outside block download

- Incoming `tx` messages are ignored early during IBD in `src/net_processing.cpp` (`PeerManagerImpl::ProcessMessage`).
- Incoming tx announcements are also effectively ignored: `AddTxAnnouncement()` is only reached from INV handling when `!IsInitialBlockDownload()`.
- `UpdatedBlockTip()` suppresses block inventory relay while `fInitialDownload` is true.
- `BlockConnected()` skips tx-download-manager updates for historical chainstates and for IBD, because the node is not populating its mempool from peers in that state.
- `MaybeSendFeefilter()` sends `MAX_MONEY` while in IBD so peers stop announcing transactions the node will discard anyway.
- `MaybeSendAddr()` skips local-address self-announcements during IBD.

## Timeouts and stalling

- Header-sync timeout handling in the send loop is primarily an IBD concern: if the best header is still at least 24 hours behind wall clock, a stalled sole sync peer can be disconnected and replaced.
- Block-stalling timeout logic is also tuned around IBD behavior. The code comment notes that stalling disconnection should mainly happen during initial block download, when the validated block window is actively moving.

## Assumeutxo interaction

- `doc/design/assumeutxo.md` and `src/validation.h` describe the two-chainstate phase: a snapshot chainstate can become the current network-tip target while a historical chainstate continues full validation toward the snapshot base.
- When the active/current chainstate leaves IBD, `Chainstate::ActivateBestChain()` records that transition and `MaybeRebalanceCaches()` gives more cache back to the background validator.
- This means a node can be out of IBD for network-tip purposes while background historical validation is still running.

## Related tests

- `src/test/validation_chainstatemanager_tests.cpp` (`chainstatemanager_ibd_exit_after_loading_blocks`)
- `test/functional/feature_maxtipage.py`
- `test/functional/p2p_ibd_txrelay.py`
- `test/functional/p2p_ibd_stalling.py`
- `test/functional/feature_assumeutxo.py`

## Adjacent pages

- `[[areas/validation-and-chainstate]]`
- `[[workflows/block-validation-and-connection]]`

## Sources consulted

- `src/validation.h` (`m_cached_is_ibd`, `UpdateIBDStatus`, `CurrentChainstate`, `HistoricalChainstate`)
- `src/validation.cpp` (`IsInitialBlockDownload`, `UpdateIBDStatus`, `Chainstate::ActivateBestChain`)
- `src/chain.h` (`CChain::IsTipRecent`)
- `src/net_processing.cpp` (`ProcessHeadersMessage`, `UpdatePeerStateForReceivedHeaders`, `HeadersDirectFetchBlocks`, `PeerManagerImpl::ProcessMessage`, `UpdatedBlockTip`, `BlockConnected`, `MaybeSendFeefilter`, `MaybeSendAddr`)
- `doc/design/assumeutxo.md`
