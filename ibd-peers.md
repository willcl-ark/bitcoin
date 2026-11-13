# IBD peers, slow peer handling and the block cache

Changes on `pr-201-peer-improv` on top of `bf25a6d3416`.

## Why

Validation on this branch is much faster than upstream, so during initial
block download the node now spends much of its time waiting for blocks from
slow peers rather than validating. These changes do three things:

- Get blocks from more peers.
- Stop asking slow peers for blocks.
- Keep downloaded blocks in memory until they are connected, so validation
  never waits on disk.

## Commits

| Commit | Summary |
|---|---|
| `8007319d82f` | net: add IBD connections, opened on a timer while asked for |
| `f1f7fe31bce` | net_processing: measure how fast each peer delivers the blocks it is asked for |
| `9c9b5a00c8b` | net_processing: stop downloading from peers that hold up initial block download |
| `b66e6bff3e8` | net_processing: a wider block download window during initial block download |
| `ea8cbceba63` | doc: release notes for IBD connections and slow block download peers |
| `d7be55dbc2e` | validation: a 4 GiB block cache during initial block download |
| `035ed86108d` | net_processing: request blocks during IBD only as far ahead as the block cache holds |
| `e56606784b2` | validation: a prefetcher reads the next blocks to connect into the block cache |
| `aa450e00925` | doc: release notes for the larger block cache and the prefetcher |

## IBD connections

- A new `ConnectionType::IBD` (`ibd` in `getpeerinfo`, `-netinfo` and the GUI).
- While in IBD and the link isn't saturated, `ThreadOpenConnections` opens
  them as fast as they can be opened, up to `-maxibdpeers` (default 50). Each
  peer has at most 16 blocks in flight, so throughput grows with the number
  of peers until the link is full; dropped and departed peers are replaced
  straight away. IBD connections don't wait for the regular outbound slots to
  fill; while those are still open the two take turns.
- `ThreadOpenConnections` only picks the address of an IBD connection. The
  attempt itself runs on one of `IBD_CONNECTION_ATTEMPTS` (10) `ibdcon`
  threads, so attempts that wait to time out don't hold up the others; no
  more are picked while 10 are in progress. While IBD connections are wanted,
  the loop picks the next after 100ms rather than 500ms. Attempts in progress
  count as connected for the IBD count and netgroup diversity.
- They have their own semaphore and file descriptor allowance, outside
  `-maxconnections`. They count towards netgroup diversity.
- They behave like block-relay-only connections: no transaction relay, no
  feefilter, no addr relay, and they are not marked as connected in addrman.
- They are disconnected when IBD ends.
- An IBD peer dropped for being slow has its address remembered
  (`AddSlowIBDPeer`), and that address is not used for another IBD connection.
- They are opened from the start of IBD, during headers sync, so they're ready
  when blocks are first requested. No new IBD connections are opened while the
  node's own link is saturated, since more peers would not help.

## Measuring peers

- Each `QueuedBlock` records when it was requested.
- On each `BLOCK` message, `RecordBlockDelivery` measures the service time
  from the later of the request time and the peer's previous delivery. This
  counts pipelined requests fairly, since a peer can only send one block at a
  time.
- Each peer's rate is an EWMA of bytes/s (`BLOCK_RATE_WEIGHT` 0.2). Each
  delivery is logged under the `net` category.
- `UpdateLinkRate` tracks the node's total download rate in 10s windows.
  `LinkSaturated()` is true when the rate has plateaued for 60s (grown less
  than 5%) and is at least 85% of the peak seen so far.

## Dropping slow peers

`CheckBlockDownloadPeers` runs from `SendMessages` at most every 250ms during
IBD:

- It finds the peer holding the lowest block in flight, which is the block
  validation is waiting for.
- That peer is flagged if the block is late and its rate is below a third of
  the median peer rate. Late means it was requested longer ago than 3× the
  time the median peer takes for a block of average size, counting it and
  each block the peer still has to send before it, and at least 1s.
- Judging only happens with at least 3 measured peers. It goes on while the
  link is saturated: a straggler then holds up a link that is full, and
  comparing peers with each other still holds.

Because peers are judged against each other rather than fixed limits, a node
on a slow connection does not drop all its peers.

`StopBlockDownloadFrom` handles a flagged peer as follows:

- **IBD connection:** disconnected and its address remembered. The timer then
  replaces it with another random peer.
- **Any other peer, including the 8 full outbounds:** stays connected but is
  not asked for blocks again until IBD ends (`m_slow_in_ibd`). With
  `-maxibdpeers=0` this is a cooldown instead, so the node isn't left with no
  peers to download from.
- Either way, the peer's outstanding requests are removed so they can be
  re-requested elsewhere. Blocks already on the connect pool are left alone.

The existing stall detection and block download timeout use the same path
during IBD instead of disconnecting.

## Blocks in flight per peer

Outside IBD a peer may have 16 blocks in flight. During IBD the limit follows
its rate (`BlocksInTransitLimit`): a peer at the median rate gets as many
blocks as it delivers in 1s, and a peer at `k` times the median gets `k²`
times that, so its blocks take about `k` seconds to arrive. Fast peers hold
most of the blocks in flight, and what a slow peer holds arrives soon, so
slow peers keep adding bandwidth instead of holding up the tip. The limit is
at least 4 and at most 64; an unmeasured peer gets 4, and every peer gets 16
until a median rate is known. Block size is the larger of the moving average
and the block just delivered, so the limit drops as soon as larger blocks
arrive; blocks already in flight stay in flight, so a peer's queue only
shrinks as it delivers. With small early blocks the limit is mostly 64.

## Hedged requests

A peer that isn't slow can still hold the next block for seconds, with other
blocks queued before it or after a pause, and validation stalls behind it.
When the lowest block in flight is late by the test above but its holder's
rate is not below a third of the median:

- It is also requested from the peer expected to deliver it soonest: the
  lowest `(blocks on the wire + 1) × average block size / rate` among peers
  that have the block, have delivered blocks, and aren't barred or paused.
  The request may take a peer one block over its in-flight limit, since peers
  fill up to their limits and none would otherwise have room.
- The first copy to arrive is used, and both requests are cleared once it's
  stored. A block is requested from at most two peers this way, and a peer
  has at most one such request waiting to be sent.
- A block that arrived from either peer and is being stored is skipped when
  looking for the lowest block in flight.
- The per-minute summary line counts these requests as `hedged`.

## Download window and block cache

- The download window is 4096 blocks during IBD (`IBD_BLOCK_DOWNLOAD_WINDOW`),
  up from 1024.
- `-blockcache` now defaults to 4096 MiB (512 MiB on 32-bit). Once IBD ends
  it is cut to at most 128 MiB.
- During IBD, blocks are only requested up to the cache horizon:
  `tip + clamp(BlocksHeld(), 16, 4096)`, where `BlocksHeld()` is the budget
  divided by the average size of the blocks cached now. Downloaded blocks
  therefore stay in memory until they are connected. They are still written
  to disk on arrival.
- `BlockCache` additions: `SetBudget`, `Contains`, `BlocksHeld` and
  `HighestTaken`.

## Prefetcher

- A thread started before block import and stopped at shutdown. It stops by
  itself when IBD ends and isn't started with `-blockcache=0`.
- Every 100ms while idle, it reads up to 16 blocks per pass from disk into the
  cache. It starts above `max(tip, HighestTaken())` and stops at the first
  block not yet downloaded, the cache horizon, or tip + 4096.
- It mainly helps after a restart mid-IBD and during reindex, when blocks are
  on disk but not in memory.
- Blocks it reads don't carry `fChecked`, so `CheckBlock` runs on them as it
  would after any disk read.

## Settings

| Option | Default | Notes |
|---|---|---|
| `-maxibdpeers=<n>` | 50 | Extra outbound connections for block download during IBD |
| `-blockcache=<MiB>` | 4096 (512 on 32-bit) | Use `-blockcache=8192` for 8 GiB |

## Observing it

- `getpeerinfo` shows connection type `ibd` for IBD peers.
- `-debug=net` logs each block delivery. Every peer dropped from block
  download is logged with the reason, without needing `-debug`.
- Whenever the lowest block in flight was requested more than 2s ago, an info
  line names it, how far above the tip it is, the peer holding it, how many
  blocks that peer has in flight, its rate and the median rate. It is logged
  once per block.
- When an IBD connection goes away, an info line gives how long it lasted,
  why (`holding up block download`, `stalling block download`, a block
  download timeout, `initial block download finished`, `before completing the
  handshake` for an attempt the peer refused or let time out, or `not by
  block download`: closed by the peer, timed out, or another reason), how many
  blocks and MB it delivered, and its rate.
- The shutdown log reports block cache hits and misses
  (`Block cache: N blocks connected from it, M not in it`) and the number of
  blocks prefetched.

## Known limitations

- Only `bitcoind` builds have been checked. No unit or functional tests have
  been written or run for these changes.
- IBD peers can still be opened when the node is limited by validation rather
  than bandwidth, if the burst while the window first filled set a high peak
  rate.
- If few addresses are available for IBD peers, the regular peers could all be
  barred from block download.
- Per-peer rates are not shown in `getpeerinfo`.
- Only the peer holding the lowest in-flight block is judged on each check.
- `HighestTaken()` does not reset after a reorg.
- Neither the cache nor peer receive buffers are counted in `-dbcache` or the
  startup memory warnings. At default settings that is up to 4 GiB plus about
  250 MB.
