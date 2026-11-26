# UTXO v3 series architecture, rationale, and future optimizations (temporary)

> **Temporary document.** This file records the architectural choices, lock and durability invariants, and deferred follow-up optimizations of the `utxo-v3` stack for reviewers and agents. Drop this commit before upstream submission.

## Goals and target environments

The `utxo-v3` stack accelerates initial block download (IBD) and reindexing across every Bitcoin Core host class: low-RAM HDD or SD nodes with 2 to 4 GiB RAM and 2 cores, mid-range desktop NVMe machines with 8 to 16 GiB RAM on Linux, macOS, and Windows, and multi-core servers with 32+ GiB RAM.

Design constraints:

- Preserve consensus rules and crash or power-loss durability guarantees.
- Avoid any tip-block speed regression for full blocks, full compact blocks, or partially reconstructed compact blocks.
- Match upstream `master` steady-state thread count, memory overhead, and P2P behavior once IBD completes.
- Keep components small, concrete, and open to future optimization work.

## Stack structure

The series sits in four layers on top of `utxo-v2/1-independent-wins` (8 commits on upstream `master`):

1. **`utxo-v2/1-independent-wins` (8 commits).** Standalone wins: unzeroed `ReadBlock` buffer, narrowed compaction error catch, capped prevout prefetch tasks, explicit file-sync tracking, `WAITED_FOR_UNDER_LOCK` checks, `GETBLOCKTXN` read-error disconnect, single-pass transaction hashing from stream bytes, and micro-wins for `PoolResource`, `SignatureCache`, `BufferedWriter`, and `FlatFileSeq::Open`.
2. **`utxo-v3/2-preparatory-refactor` (4 commits).** Script execution cache lock with lock-free empty fast path; pure `ConnectBlock` and `DisconnectBlock` over `BlockValidationContext` with flat-input `std::span<const Coin>` overloads, a `have_any_output` callback for BIP30, and stack-local intra-block `spent_table`; off-`cs_main` block checks and writes with a persistent `blk*.dat` write handle and single `GetSerializeSize()` measurement; non-virtual `CCoinsViewCache`, zero-copy `CCoinsView::SortedCoinWrite` and `CCoinsViewDB::BatchWriteSorted()`, preserved LevelDB block cache with snapshot-cursor `ComputeUTXOStats`, and `ThreadPool::SubmitFront()`.
3. **`utxo-v3/3-rearchitecture` (3 commits).** Chunked 256-shard `CoinsStore` with separate 36 B tombstones, O(dirty) `Sync()`, double-buffered parallel flush sorting, and 2-phase commit support; lazy-started stoppable single-pass async `UndoWriter` with persistent `rev*.dat` handle and ordered prune unlinks; 64-shard `WindowOutputs` with rotating shard traversal and bounded sweep, plus 2-phase `ConnectWindow` sharing `m_connect_pool` and releasing IBD resources on IBD exit.
4. **`utxo-v3/4-async-p2p-handoff` (1 commit).** IBD-only lazy-started and idle-stoppable `BlockConnector` (`blockconn`), a bounded FIFO P2P `CBlock` handoff cache, per-peer bounded deferred `PONG`s, IBD/prune-scoped download lookahead clamp, and `ProcessOrphanTx` early-out. No separate `blockrecv` pool.

## Core design decisions and rationale

### Chunked 256-shard `CoinsStore`

Files: `src/coinsstore.h`, `src/coinsstore.cpp`, `src/test/coinsstore_tests.cpp`

`CoinsStore` inherits `CCoinsViewBacked`, not `CCoinsViewCache`. Keeping `CCoinsViewCache` concrete and non-virtual avoids 14 virtual methods, avoids carrying an unused `CCoinsMap` and `PoolResource`, and removes the `AccessCoin()` reference-stability trap. Callers on `CoinsTip()` use `GetCoin()`, `PeekCoin()`, `HaveCoin()`, or `CCoinsView` directly.

Key layout and algorithmic properties:

- **256 key-range shards.** Leading txid byte selects the shard, so shards are disjoint contiguous key ranges. Iterating shards in order yields globally sorted LevelDB keys without a cross-shard merge.
- **Fixed 64-entry `LiveChunk`s.** Each shard stores live coins in heap-allocated 64-entry chunks (`std::vector<std::unique_ptr<LiveChunk>>`) paired with a power-of-2 open-addressing `std::vector<uint32_t>` index table using backward-shift deletion and O(1) swap-with-last removal. Using fixed chunks eliminates `std::vector` doubling cliffs and copy stalls at multi-gigabyte cache sizes. At 5,648 bytes per chunk, worst-case slack across 256 shards is 1.4 MiB, which keeps small `-dbcache` nodes responsive.
- **88-byte `PackedEntry` with full 31-bit height.** Each live slot holds a 36-byte `COutPoint`, a 4-byte `code = (nHeight << 1) | fCoinBase`, and a 48-byte `CTxOut`. Upstream `master` already sets `CScriptBase = prevector<36, uint8_t>`, so P2PKH, P2SH, P2WPKH, P2WSH, and P2TR scripts fit inline with zero heap allocation. Each chunk holds two `uint64_t` bitsets (`dirty` and `fresh`).
- **Separate 36-byte `TombChunk`s.** Spent non-fresh coins are recorded as 36-byte `COutPoint` tombstones in 64-entry `TombChunk`s, distinguished in the index table by `TOMB_BIT` (`1u << 31`). Tombstones take less than half the memory of a full `PackedEntry`, so spend-heavy IBD eras trigger fewer flushes on low-RAM hosts.
- **O(dirty) tip `Sync()`.** Each shard maintains a level-2 `dirty_chunk_mask` bitset over its `LiveChunk`s. During `Sync()`, clean chunks are never visited. Dirty bits are cleared only on flagged chunks, and when `tomb_count * 8 < index.size()`, tombstones are removed in place via backward-shift deletion without rehashing the index table.
- **Double-buffered parallel flush sorting.** When `dirty_count >= 4096` and `m_connect_pool` has active workers, `WriteDirtyToBase()` sorts up to `clamp(workers * 2, 2, 16)` shards in parallel into compact 16-byte `SortKey` arrays while the calling thread streams the previously sorted batch into `CCoinsViewDB::BatchWriteSorted()` using zero-copy `SortedCoinWrite` pointers.
- **Shard-batched `BatchWrite()`.** Serial `ConnectTip()` keeps upstream `CoinsViewOverlay` directly over `CoinsStore` (no `BlockApplyView` wrapper). `CoinsStore::BatchWrite()` buckets dirty cursor entries by shard first and locks each shard once.
- **3-state `FetchInto()` and sorted misses.** `FetchInto()` returns `FETCH_ABSENT`, `FETCH_FOUND`, or `FETCH_SPENT`, so window workers query LevelDB only for `FETCH_ABSENT` misses, sorted by `COutPoint` to improve SSTable block-cache locality.

### 2-phase check-then-apply `ConnectWindow`

Files: `src/node/window_outputs.h`, `src/node/window_outputs.cpp`, `src/node/connect_window.h`, `src/node/connect_window.cpp`, `src/validation.cpp`

During IBD on machines with 3+ cores (`worker_threads_num >= 2`, or an explicit `-connectwindow=N` with `worker_threads_num >= 1`), `ConnectWindowStep()` validates up to `m_connect_window` blocks in parallel on `m_connect_pool`:

1. **Read and index.** `RunConnectTask()` takes a parsed `CBlock` from the P2P handoff cache if present, otherwise reads and parses from `blk*.dat` under `m_block_read_sem` (4 permits). It indexes all outputs into `WindowOutputs` (64 shards). Every call to `WindowOutputs::Index()`, `FindInputs()`, `NamedIds()`, and `Sweep()` advances an atomic `m_shard_cursor` to start at a different shard, breaking 64-shard mutex convoys across workers.
2. **Validate without a `CCoinsViewCache`.** Once every lower block in the window is indexed, `RunWindowValidation()` resolves inputs via `ResolveWindowInputs()` (`WindowOutputs` -> 3-state `CoinsStore::FetchInto()` -> key-sorted `CoinsStore::BaseCoin()`), then calls the flat-input `::ConnectBlock()` overload directly with `BlockValidationContext::input_coins` and `have_any_output` (calling `WindowHaveAnyOutput()`). All temporary buffers (`input_coins`, `from_base`, `spent_table`, `groups`) are stack-local so no `thread_local` high-water vectors linger after IBD. On success, `PrepareCommit()` builds compact 16-byte `CommitItem`s (constructing `own_spent` lazily only when a block spends its own outputs), `SerializeBlockUndo()` serializes the undo data into `s.serialized_undo`, and `result.undo` is destroyed immediately so no parsed `CBlockUndo` stays resident in the window.
3. **2-phase parallel commit (zero rollback).** When the lowest block finishes, `ConnectWindowStep()` takes `cs_main` and starts a `CommitJob` covering up to 32 contiguous ready blocks (8 when cache is `LARGE`, 1 when `CRITICAL` or near flush/prune). Commit helpers are submitted with `m_connect_pool->SubmitFront()` so they preempt queued read/validation tasks while `cs_main` is held:
   - **Phase 1 (`CheckShard`).** Workers bucket items per shard in block order and verify every spend and non-overwrite create read-only against `CoinsStore` (using `resolve_flush_gen == store_flush_gen` plus `FROM_BASE` to skip redundant LevelDB reads on untouched base coins, and sorting any untrusted misses). Each group records the earliest conflicting block index in `job->first_conflict`. No shard is mutated.
   - **Barrier.** Once all groups finish Phase 1, `valid_blocks = has_error ? 0 : min(blocks.size(), first_conflict)` is published and all threads wake on `sync.cv`.
   - **Phase 2 (`ApplyShardUnconditional`).** Workers apply blocks `0..valid_blocks - 1` unconditionally across disjoint shard groups. Because Phase 1 already verified the prefix and mutated nothing on error or conflict, no `TakeBackCommit()` rollback path exists. Any block at or after `first_conflict` is marked `commit_failed` and retried serially in `ActivateBestChain()`.
4. **Publish and bounded cleanup.** `PublishWindowBlock()` calls `FinishConnectBlock(..., async_undo=true)` using `s.serialized_undo`, updates `CoinsTip().SetBestBlock()`, queues `BlockConnected` on `UndoWriter`, and marks the height released in `WindowOutputs`. `FlushStateToDisk()` runs only on the last block of the committed batch, when `CoinsStore` matches `best_block`. Off `cs_main`, `WindowOutputs::Sweep(4)` bounds per-call cleanup work, and an RAII `WindowDrainGuard` drains `in_flight` and resets `m_window_active_bytes` on every exit path off `cs_main`.

### Memory budgeting and low-end host scaling

Files: `src/node/connect_window.h`, `src/validation.cpp`

- **Single-threaded / 2-core default.** `-connectwindow` is `DEBUG_ONLY`. When `worker_threads_num <= 0` (`-par=1` or kernel with 0 workers), `m_connect_window` is always `1`. When `worker_threads_num == 1` (2-core machines), default `m_connect_window` is `1` to avoid parallel window overhead unless `-connectwindow` is passed explicitly. On 3+ cores, `m_connect_window = clamp(worker_threads_num * 2, 1, 64)` with `clamp(worker_threads_num, 2, min(m_connect_window, 16))` pool workers.
- **Scaled budgets.** Window bytes (`clamp(cache / 16, 4 MiB, 256 MiB)`), pinned `BlockConnected` bytes (`clamp(cache / 16, 4 MiB, 256 MiB)`), undo backlog (`clamp(cache / 16, 8 MiB, 64 MiB)`), and P2P handoff cache (`clamp(total_cache / 32, 4 MiB, 32 MiB)`) all scale down on low `-dbcache` hosts.
- **Unified RSS accounting.** `Chainstate::GetCoinsCacheSizeState()` deducts `PendingUndoBytes() + m_pinned_block_bytes + m_window_active_bytes + HandoffBlockBytes()` from `m_coinstip_cache_size_bytes` (floored at half the cache budget) so auxiliary IBD buffers cannot silently push RSS past `-dbcache`.
- **Keep LevelDB block cache.** `CCoinsViewDB::ResizeCache()` is removed; reopening LevelDB during assumeutxo cache rebalancing flushed OS/LevelDB caches unnecessarily. `ComputeUTXOStats()` now takes a LevelDB snapshot cursor so it does not hold stale cache state.

### Persistent block/undo handles and lazy `UndoWriter`

Files: `src/node/blockstorage.h`, `src/node/blockstorage.cpp`, `src/validation.cpp`

- **Persistent `blk*.dat` write handle.** `BlockManager::WriteBlockAt()` reuses a single `AutoFile` handle guarded by `m_block_write_mutex` across consecutive writes to the same block file, closing it before file switches, chunk preallocation, trims, flushes/syncs, prune unlinks, and shutdown. This eliminates repeated `open`/`close` syscalls and per-close antivirus rescans on Windows. `PreparedBlock::block_size` passes the exact serialized size from `AllocateBlockPos()` so `WriteBlockAt()` does not call `GetSerializeSize()` a second time.
- **Lazy, stoppable single-pass `UndoWriter`.** `SerializeBlockUndo()` writes header, `CBlockUndo`, and checksum in one pass, shared by inline `WriteBlockUndo()` and async `QueueBlockUndo()`. The `undowriter` thread starts lazily on the first async job and stops cleanly via `StopUndoWriter()` on IBD exit or shutdown. `WriteSerializedUndo()` honors `-test=crash_before_undo_write` on both inline and async paths. Any non-write job (prune unlinks or `TrimUndoFile` callbacks) closes the persistent `rev*.dat` handle first, and `TrimUndoFile()` routes through the undo queue whenever `undowriter` is active or `m_undo_pending` is non-empty, preventing open-descriptor races on Windows and POSIX.
- **No fsync under `cs_main` on file rotation.** Block and undo file rotation trims files without calling fsync under `cs_main`. Durability is enforced by `SyncWrittenFiles()` before `WriteBlockIndexDB()`.

### IBD-only `BlockConnector` and bounded P2P `CBlock` handoff cache

Files: `src/node/block_connector.h`, `src/node/block_connector.cpp`, `src/net_processing.cpp`, `src/validation.cpp`

- **No `blockrecv` pool.** P2P `BLOCK` deserialization and storage stay on `msghand` (where `AcceptNewBlock()` and `WriteNewBlock()` already run checks and disk writes off `cs_main`). Dropping `blockrecv` removes 4 threads, eliminates out-of-order concurrent writes inside NTFS preallocated files on Windows, and removes `with_pool` tracking in `mapBlocksInFlight`.
- **Bounded P2P `CBlock` handoff cache.** During IBD, `FinishNewBlock(..., activate_async=true)` stores the parsed `CBlock` in a bounded FIFO cache (`clamp(total_coinstip_cache / 32, 4 MiB, 32 MiB)`) before signaling `BlockConnector`. `ConnectWindowStep()` and `ConnectTip()` pop blocks from this cache first, avoiding redundant `blk*.dat` reads and deserialization. At tip, the cache is empty and `TakeHandoffBlock()` returns immediately via a relaxed atomic check with zero locks.
- **Idle-stoppable `BlockConnector`.** `blockconn` starts lazily on the first IBD handoff. `StopForIdle()` joins it cleanly once IBD (and any assumeutxo historical chainstate) completes while still allowing a future `Signal()` to restart it before permanent `Stop()` at shutdown. `MaybeReleaseIBDResources()` skips `StopForIdle()` if called from `blockconn` itself (`IsConnectorThread()`), letting `msghand` finish joining `blockconn` on the wakeup emitted by `m_on_active_done`.
- **Per-peer bounded deferred `PONG`.** `Peer::m_last_handoff_ticket` records the connector ticket only when that specific peer hands off an IBD block. `Peer::m_pending_pongs` is capped at 8 entries per peer (replying to the oldest entry if full). Peers that have not handed off blocks and all post-IBD tip `PING`s reply inline without acquiring `BlockConnector`'s mutex.
- **Scoped lookahead clamp.** `FindNextBlocksToDownload()` clamps `nWindowEnd` to `min(pindexLastCommonBlock->nHeight, ActiveHeight()) + download_window` only during IBD or in prune mode, leaving post-IBD non-pruning fork downloads unchanged.

### Post-IBD tip fast path and thread parity

Once `IsInitialBlockDownload()` latches to `false` (and `HistoricalChainstate()` is null), `MaybeReleaseIBDResources()` runs off `cs_main`:

1. Settles and joins `undowriter` (`StopUndoWriter()`).
2. Clears the P2P handoff cache.
3. Resizes `m_connect_pool` back to `prevoutfetch_threads_num`.
4. Joins `blockconn` (`StopForIdle()`, executed off `blockconn` via `msghand` or the activating thread).

In steady state at tip:

- Thread count matches upstream `master` (`m_script_check_queue` plus `prevoutfetch_threads_num` workers in `m_connect_pool`; zero `undowriter`, zero `blockconn`, zero `blockrecv`).
- `ProcessBlock()` runs synchronously on `msghand` with `activate_async = false`.
- `ConnectTip()` uses `CoinsViewOverlay` over `CoinsStore` with inline `WriteBlockUndo()` and inline `UpdatedBlockTip()`, never starting `undowriter`.
- Automatic prune unlinks post-IBD run inline in `FlushStateToDisk()`, so `undowriter` stays stopped.
- `PING` and `TakeHandoffBlock()` take zero extra locks.

### Lock, wait, and durability order

Lock and wait hierarchy (outermost first):

- `m_chainstate_mutex` > `cs_main` > `m_mempool->cs` > `WindowSync::mutex` > `CoinsStore::Shard::mutex`
- `m_pool_lifecycle_mutex` > `cs_main` (never called while holding `cs_main` or `m_chainstate_mutex`)
- `m_connect_pool` worker tasks (`RunConnectTask`, `RunWindowValidation`, `RunCommitWork`, flush sort tasks, prevout prefetch tasks) never acquire `cs_main`.
- `BlockConnector` never calls `StopForIdle()` on itself (`IsConnectorThread()`).

Every flush in `FlushStateToDisk()` enforces:

1. `SettleUndoWrites()` drains all queued undo I/O, closes any open `rev*.dat` handle, and applies completions under `cs_main`.
2. `FlushChainstateBlockFile()` and `SyncWrittenFiles()` close any open `blk*.dat` write handle and fsync every file in `m_unsynced_block_files` and `m_unsynced_undo_files`.
3. `WriteBlockIndexDB()` persists block index and file metadata synchronously.
4. Pruned files are unlinked only after the block index records the prune (inline for manual `pruneblockchain` and post-IBD; on `undowriter` during IBD).
5. `CoinsTip().Flush()` or `CoinsTip().Sync()` writes the UTXO set to LevelDB.

Any block or undo write/sync failure latches permanently (`m_sync_failed`, `m_undo_write_failed`) and prevents further block index or UTXO flushes.

## Deferred follow-up optimizations

These optimizations were intentionally left out of `utxo-v3` to keep the series small and reviewable while leaving clean extension points:

1. **Off-`cs_main` `SyncWrittenFiles()`.** Fsyncing rotated `blk*.dat` and `rev*.dat` files during flush still runs under `cs_main`, matching upstream. Moving fsync off `cs_main` (while guarding against concurrent unlinks) would cut `cs_main` stalls on slow storage and macOS `F_FULLFSYNC`.
2. **Sub-block script parallelism for `-assumevalid=0` and the post-assumevalid tail.** `RunWindowValidation()` passes `check_queue = nullptr`, running each block's scripts on one worker while parallelizing across blocks. Sharing `m_script_check_queue` across window workers would remove tail-latency stalls when one large block dominates the window.
3. **Positional I/O (`pread`/`pwrite`) and readahead hints in `FlatFileSeq`.** Replacing stdio `FILE*` with positional OS calls (`pread`/`pwrite` on POSIX, overlapped `ReadFile`/`WriteFile` on Windows) would allow lock-free shared read descriptors across `m_connect_pool` workers and cleaner `posix_fadvise`/`F_RDAHEAD` hints.
4. **Compressed scripts and slab arena in `CoinsStore`.** `PackedEntry` is 88 bytes today because `CTxOut` holds a full `CScript` (`prevector<36, uint8_t>`). Storing `ScriptCompression`-style compact script tags or variable-length script payloads in a per-shard slab could cut UTXO cache RAM by another 20 to 30%.
5. **Non-blocking copy-on-write `Flush()`.** `CoinsStore::Flush()` still holds `cs_main` while streaming sorted batches to LevelDB. Generation-tagged shard snapshots or a foreground delta layer could overlap LevelDB flushes with block validation.
6. **Post-IBD assumeutxo background windowing.** `ConnectWindowStep()` currently gates on `IsInitialBlockDownload()`. Allowing the historical background chainstate to keep using `ConnectWindow` after the active snapshot tip exits IBD (while keeping active-tip P2P blocks synchronous) would accelerate assumeutxo background validation.
7. **Adaptive read semaphore and window width.** `m_block_read_sem` is fixed at 4 permits. Measuring read latency versus validation time to throttle down to 1 to 2 readers on HDD/SD and scale up on NVMe would avoid manual tuning.
8. **Per-block arena allocation for `CBlock`.** Deserializing and freeing millions of `CTransaction`/`CTxIn`/`CTxOut` vectors creates allocator churn during IBD. A contiguous per-block arena would replace thousands of small allocations per block with one allocation and one free.
