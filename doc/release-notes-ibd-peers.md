P2P and network changes
-----------------------

- During initial block download the node now makes extra outbound
  connections, one every 10 seconds up to `-maxibdpeers` (default: 50), used
  only to download blocks. They relay neither transactions nor addresses,
  are counted separately from `-maxconnections`, and are closed once initial
  block download ends. They show up with the connection type `ibd` in
  `getpeerinfo` and `-netinfo`. No more are opened while the node's own
  connection is already saturated.

- During initial block download, a peer holding the block that block
  download waits for while serving blocks much more slowly than the other
  peers is no longer asked for blocks: an `ibd` connection is closed and
  replaced, and any other peer is kept connected but is not asked for blocks
  again until initial block download ends. The same applies to peers that
  stall block download or time out on a block during initial block download,
  instead of disconnecting them. Peers are judged against each other, not
  against fixed limits, and not at all while the node's own connection is
  saturated, so nodes on slow connections are not affected.

Updated settings
----------------

- `-blockcache` now defaults to 4096 MiB (512 MiB on 32-bit platforms).
  During initial block download blocks are only requested as far ahead of
  the tip as the cache can hold, so that they are connected from memory
  rather than read back from disk. Once initial block download ends the
  cache is cut to at most 128 MiB.

- During initial block download, blocks that are stored but not in the
  block cache (for example after a restart, or during a reindex) are read
  into it ahead of validation by a prefetcher thread.
