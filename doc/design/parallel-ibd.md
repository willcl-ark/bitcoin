# Parallel Initial Block Download (IBD) Architecture (`utxo-v4`)

## 1. Goals and scope

This document describes the architecture, invariants, verification coverage, and deliberate scope boundaries of the parallel IBD implementation in `utxo-v4`, a compact rewrite building on `sedited`'s `pr-201` (`utxo-partial-order`) architecture and concepts.

The goal is a reviewable, consensus-identical parallel IBD pipeline that:

- Achieves parity on block-connect throughput with larger parallel UTXO rewrites while keeping the diff compact (under half the size of `pr-201`'s ~22,300 added lines across 145 files; see Section 4 for benchmarked snapshot diff sizes).
- Preserves Bitcoin Core's existing post-IBD tip behavior, P2P relay semantics, and RPC/mempool consistency (`ShardedCoinsView` never runs ahead of `ActiveTip()` while `cs_main` is unlocked).
- Bounds all auxiliary IBD memory explicitly and tears down all IBD-only threads, caches, and file handles automatically upon leaving IBD.
- Keeps the core data structures and stage boundaries clean enough that every remaining optimization can be landed as an isolated follow-up PR without re-architecting validation or storage.

---

## 2. High-level architecture

The branch splits IBD work across five cooperating components:

1. **Concurrent key-range-sharded UTXO cache (`ShardedCoinsView`, `src/coins_sharded.{h,cpp}`)**
   - Replaces `CCoinsViewCache` as the active chainstate tip cache (`CoinsTip()`).
   - 256 shards indexed by the leading byte of `txid` (`ShardIndex(outpoint) = txid[0]`), matching LevelDB's serialized key order so `Flush()` and `Sync()` can sort each shard independently in parallel and stream in strict key order.
   - Each shard protects an open-addressed `IndexSlot` table (8 bytes/slot, 80% max load factor) pointing into 2 MiB-aligned `MADV_HUGEPAGE` slabs (`MAX_SLAB_SHIFT = 15`, 32,768 entries/slab) of 80-byte `LiveEntry` records and separate 40-byte `TombstoneEntry` records, under a `std::shared_mutex`.
   - `StoredCoin` packs amounts, height/coinbase flags, and scripts inline up to 32 bytes (`StoredCoin::INLINE = 32`, covering P2PKH, P2SH, and P2WPKH directly) plus compact 32-byte encodings for standard P2WSH, P2TR, and compressed P2PK scripts, avoiding heap allocations for >99% of mainnet outputs.
   - Maintains a monotonic `m_flush_epoch` incremented on `Flush()`, tombstone-removing `Sync()`, and `Uncache()` so batch commits can verify that speculative LevelDB reads are still valid.

2. **Decoupled block validation core (`ValidateBlockCore` / `ValidateBlockWithFlatInputs`, `src/validation.{h,cpp}`)**
   - Extracts context-independent and contextual transaction checking, sigops counting, BIP30 checks, script verification, and optional `CBlockUndo` assembly into a pure function over immutable `BlockValidationContext` + pre-resolved flat input arrays, with no `ChainstateManager`, UTXO store, or thread pool dependency (tested standalone in `validation_block_tests/validate_block_with_flat_inputs_standalone`).
   - Skips `CBlockUndo` construction when `!ctx.build_undo` (for example during `-reindex-chainstate` when `rev*.dat` already exists; real IBD always builds, serializes, and writes full undo data).
   - Shares the exact same consensus checks between serial `ConnectBlock()` and the parallel IBD window.

3. **Bounded 4-stage parallel IBD window (`ParallelIBDWindow` + `WindowOutputTable`, `src/ibd_window.{h,cpp}`, `src/validation.cpp`)**
   - `ActivateBestChainParallelIBD()` drives a sliding window ahead of `ActiveTip()` while in IBD (`window_width = clamp(cores * 4, 16, 512)`, `max_batch = clamp(window_width / 2, 1, 64)`, worker pool scaling up to `MAX_PREVOUTFETCH_THREADS = 256` on large-core hosts while `IBDMemoryBudget` bounds memory proportionally to `-dbcache`).
   - **Stage 1 (Load & Deserialize, worker pool):** Checks `BlockHandoffCache` first; on miss, reads and decodes `blk*.dat` lock-free via platform-agnostic `BlockManager::ReadBlockConcurrent()` (backed inside `src/node/blockstorage.cpp` by a cached 16-FD POSIX `pread()` pool with 4 KiB stack fast path, `POSIX_FADV_SEQUENTIAL` on open, and coalesced `BlockManager::AdviseBlockRange()` / `POSIX_FADV_DONTNEED` FD eviction dispatched on an RAII-guarded pool task off the coordinator thread) and hashes transactions directly from serialized wire bytes (`src/primitives/txsegments.h`).
   - **Stage 2 (Index Outputs, worker pool):** Inserts the block's `vtx` pointers into a keyed-hash (`SipHasher13UJ`), zero-copy open-addressed per-txid `WindowOutputTable` (64 shards, dynamically sized power-of-2 `CBlock` ring buffer). Advances `m_indexed_watermark` contiguously and immediately chains the earliest newly unblocked slot into Stage 3 on the same worker thread.
   - **Stage 3 (Speculative Resolution & Validation, worker pool):** Captures `validated_flush_epoch = m_coins_tip.FlushEpoch()`, resolves intra-block spends via a salted thread-local generational open-addressed table, resolves external inputs across 3 layers (`WindowOutputTable` -> `ShardedCoinsView::LookupBatch` -> `CCoinsViewDB::PeekCoin` with empty-DB fast path `m_has_coins`), runs batched 3-layer BIP30 checks when `ctx.enforce_bip30`, executes `ValidateBlockWithFlatInputs()`, and builds pre-sharded zero-copy `BlockSpendOp` / 16-byte POD `BlockCreateOp` vectors.
   - **Stage 4 (Single-Pass Undo Serialization, worker pool):** Immediately serializes `CBlockUndo` into a compact `SerializedBlockUndo` payload when `needs_undo` is true and drops the in-memory `CBlockUndo`.
   - **Ordered Batch Commit (coordinator under `cs_main`):** Commits a contiguous `READY` prefix (up to `max_batch` blocks) via `ShardedCoinsView::CommitBlockBatch()`. Shards execute in parallel across `m_validation_pool` when `total_ops >= 4096`. On any cross-block conflict or stale BIP30 flush epoch at block `c`, shards roll back `c..end` in reverse order, publish `0..c-1`, drain the window, and fall back to serial `ConnectTip()` for block `c` so consensus verdicts and failure flags match serial execution identically.
   - **Off-coordinator teardown:** Committed `WindowOutputTable` heights are lazily erased by workers via `Sweep()`, and committed `Slot` objects are destroyed on a pool worker rather than on the coordinator thread.

4. **Off-`cs_main` block storage and single-pass async undo writer (`src/node/blockstorage.{h,cpp}`, `src/validation.cpp`)**
   - `ChainstateManager::StoreNewBlock()` runs `CheckBlock()`, `ContextualCheckBlock()`, disk serialization, and `WriteRawBlockToDisk()` off `cs_main`, taking brief `cs_main` locks only for header acceptance, file-position reservation, and `ReceivedBlockTransactions()`.
   - `NewPoWValidBlock` fires in Step 7 under `cs_main` only after `ReceivedBlockTransactions()` sets `BLOCK_HAVE_DATA` and `HaveNumChainTxs()`, guaranteeing peers replying immediately with `GETDATA` or `GETBLOCKTXN` never observe a partially stored block.
   - `AsyncUndoWriter` streams pre-serialized `rev*.dat` payloads on a single lazy, stoppable background thread (`undowrite`). `FlushStateToDisk()` enforces a strict 5-step durability barrier:
     1. `SettlePendingUndoWrites()` (drains undo queue, closes persistent `rev*.dat` handle, applies `BLOCK_HAVE_UNDO`)
     2. `FlushUnsyncedFiles()` (`fsync` all modified `blk*.dat` and `rev*.dat` files)
     3. `WriteBlockIndexDB()` (persist block index and pruned file flags to LevelDB)
     4. `UnlinkPrunedFiles()` (unlink pruned files only after LevelDB records them pruned)
     5. `CoinsTip().Flush()` / `Sync()` (persist UTXO tip to LevelDB)
   - Any write or sync I/O failure permanently latches `m_storage_io_error` and refuses further flushes so crash recovery never advances `CoinsDB` past missing block/undo files.

5. **IBD-only P2P handoff, `ibdconnect`, and automatic tip teardown (`src/net_processing.cpp`, `src/validation.cpp`)**
   - During IBD, `PeerManagerImpl::ProcessBlock()` runs `StoreNewBlock()` on `msghand`, places the parsed `CBlock` into a bounded `BlockHandoffCache`, and signals a single lazy, idle-stoppable `ibdconnect` thread with a monotonic handoff ticket.
   - `PING` replies from peers with an unfinished IBD block handoff ticket are deferred (capped at `MAX_DEFERRED_PONGS_PER_PEER = 16`) until `m_ibd_completed_ticket` catches up, the handed-off block enters `ActiveChain()` (waking `msghand` from `ActiveTipChange`), or the 15-second liveness cap (`DEFERRED_PONG_MAX_WAIT`) elapses—preserving causal ordering for tests and peers without stalling `msghand` or timing out peers during long validation-bound IBD.
   - `FindNextBlocksToDownload()` bounds lookahead relative to `ActiveHeight() + BLOCK_DOWNLOAD_WINDOW` during IBD once `state->pindexLastCommonBlock->nChainWork > ActiveTip()->nChainWork`, while `FindNextBlocks()` suppresses false stall accusations when the local `ibdconnect` backlog rather than a remote peer is what caps the window.
   - Post-IBD, `ProcessBlock()` takes the synchronous `ProcessNewBlock()` fast path with zero extra locks, and `MaybeTeardownIBDResources()` stops `ibdconnect`, `AsyncUndoWriter`, closes persistent write handles, clears `BlockHandoffCache`, and tears down the IBD window.

---

## 3. Correctness invariants and verification

Key correctness invariants enforced across the implementation and verified by the in-tree test and fuzz suite:

- **Serial equivalence & cache flag soundness:** `ShardedCoinsView` overlaid on LevelDB always matches strict serial consensus state (`FRESH => LIVE /\ DIRTY /\ db[k] = NONE`, `TOMB => DIRTY`, clean `LIVE` matches DB); every speculative validation failure, stale BIP30 flush epoch, or assumevalid change at commit time falls back to serial `ConnectTip()`.
- **Crash durability & pruning order:** 5-step `FlushStateToDisk()` barrier guarantees `rev*.dat`/`blk*.dat` fsync completes before LevelDB `blocks/index` records `BLOCK_HAVE_UNDO`/pruned flags, which completes before unlinking pruned files and advancing `CoinsDB`. Any storage I/O error latches permanently and invokes fatal `flushError()`.
- **IBD connector & resource lifecycle:** `ibdconnect` stops before scheduler shutdown, never self-joins, bounds deferred `PONG`s at 16 per peer while preserving causal ordering, suppresses false download-stall accusations when local connect lags, and tears down all IBD-only threads, caches, and read handles on IBD exit.

In-tree test coverage spans every commit in the series:
- **C++ unit tests:** `txhashing_tests`, `threadpool_tests` (`SubmitPriority`, `Resize`, off-lock task teardown), `dbwrapper_tests` (snapshot iterator isolation and `CheckStatus`), `coins_tests` (`BatchWriteEntries`, `FindCoinByTxid`, `m_has_coins` reload), `txvalidationcache_tests` (concurrent `ValidationCache`, atomic `CBlock` flags, `stopAtHeight`), `validation_block_tests` (standalone `ValidateBlockWithFlatInputs` consensus matrix, `ShouldEnforceBIP30` boundaries, `StoreNewBlock` idempotency and I/O latch), `sharded_coins_tests` (1,500-step differential model check against `CCoinsViewCache`, parallel `>= 4096` op `ShardedBlockCommit` + full `RestoreKind` rollback, BIP30/stale-epoch conflicts, concurrent reader/writer stress), `validation_flush_tests`, `flatfile_tests` (`WillAllocate`/`Trim`/`Sync`), `blockmanager_tests` (off-`cs_main` writes, 8-thread `ReadBlockConcurrent`, `EnqueueBlockUndoWrite` vs `StopAsyncUndoWriter` race, fault injection), `validation_chainstatemanager_tests` (`IBDMemoryBudget`, `BlockHandoffCache`, `IBDExitCallbackRegistration` lifetime, explicit `-prevoutfetchthreads`), `ibd_window_tests` (`WindowOutputTable` visibility/BIP30/ring wrap, forced-parallel vs serial UTXO equivalence, invalid scripts, conflicts, `-stopatheight`, fault injection, teardown), and `test_kernel`.
- **Fuzzing:** `src/test/fuzz/sharded_coins_window.cpp` differential oracle fuzzing `ShardedCoinsView` against `CCoinsViewCache`.
- **Python functional tests:** `feature_async_undo.py`, `feature_connect_window.py`, `p2p_failed_block_not_served.py`, `p2p_ibd_lookahead_and_stalling.py`, `p2p_ibd_block_handoff.py`, `p2p_block_source_duplicate.py`, and `p2p_ibd_exit_and_tip_path.py`.

---

## 4. Mainnet SSD benchmark profile (`utxo-v4` vs `pr-201`)

Measured on an idle 8-core / 16-thread host (`-reindex-chainstate -stopatheight=975073 -dbcache=32000 -par=16 -prevoutfetchthreads=16 -networkactive=0`, GCC 16.2.0, `RelWithDebInfo`, THP `madvise`):

| Branch / commit | Diff size | `connect_s` | `wall_s` | `0 -> 300k` | `300k -> 966,144` | `966,144 -> 975,073` | Final flush | Peak RSS |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `utxo-v4` (`b5ecfed85d0`, pre-fix) | `+9,055 / -761` | 1,387.7 s | 1,468.5 s | 23.7 s | 1,054.3 s | 309.7 s | 80.8 s | 21.3 GiB |
| `utxo-v4-series` (`b5b70e83c47`, pre-fix) | `+9,231 / -693` | 1,422.8 s | 1,503.0 s | 24.9 s | 1,076.7 s | 321.1 s | 80.2 s | 21.3 GiB |
| **`utxo-v4-series` (`f2fbb6de868`)** | **`+9,279 / -694`** | **1,232.9 s** | **1,312.8 s** | **20.9 s** | **902.2 s** | **309.8 s** | **79.9 s** | **20.9 GiB** |
| `pr-201` (`d2b8b9b167b`) | `+22,323 / -1,064` | 1,231.6 s | 1,277.1 s | 35.7 s | 887.9 s | 308.0 s | 45.5 s | 19.7 GiB |
| Delta (`f2fbb6de868` vs `pr-201`) | `-13,044` lines | `+1.3 s` (+0.10%) | `+35.7 s` (+2.8%) | `-14.8 s` (-41.5%) | `+14.3 s` (+1.6%) | `+1.8 s` (+0.58%) | `+34.4 s` | `+1.2 GiB` |

Phase-by-phase breakdown of `connect_s` (`f2fbb6de868` vs `pr-201`):

| Height range | `utxo-v4` (`f2fbb6d`) | `pr-201` | Delta | Ratio |
| --- | ---: | ---: | ---: | ---: |
| `0 -> 100k` | 1.0 s | 3.7 s | -2.7 s | 0.27x |
| `100k -> 200k` | 3.8 s | 10.5 s | -6.7 s | 0.36x |
| `200k -> 300k` | 16.1 s | 21.4 s | -5.4 s | 0.75x |
| `300k -> 400k` | 48.0 s | 49.4 s | -1.4 s | 0.97x |
| `400k -> 500k` | 104.6 s | 101.0 s | +3.6 s | 1.04x |
| `500k -> 600k` | 111.9 s | 111.2 s | +0.7 s | 1.01x |
| `600k -> 700k` | 139.1 s | 138.4 s | +0.7 s | 1.00x |
| `700k -> 800k` | 152.1 s | 143.9 s | +8.2 s | 1.06x |
| `800k -> 850k` | 104.3 s | 109.4 s | -5.2 s | 0.95x |
| `850k -> 900k` | 105.5 s | 102.6 s | +2.9 s | 1.03x |
| `900k -> 950k` | 99.9 s | 96.3 s | +3.6 s | 1.04x |
| `950k -> 966,144` | 36.9 s | 35.8 s | +1.1 s | 1.03x |
| `966,144 -> 975,073` (scripts) | 309.8 s | 308.0 s | +1.8 s | 1.01x |

Three observations stand out:

- Fixing `ThreadPool::WorkerThread()` to destroy completed `std::packaged_task` captures outside `m_mutex` and moving `POSIX_FADV_WILLNEED` / `ReadFd` eviction off the coordinator thread recovered `154.8 s` (`-11.2%`) of connect time, raised median `nvme0n1` throughput from `689 MB/s` to `808 MB/s`, and trimmed peak RSS by `0.4 GiB`.
- `utxo-v4` now beats `pr-201` through height `966,144` (`923.1 s` vs `923.6 s`) and matches `pr-201` on total `connect_s` (`1,232.9 s` vs `1,231.6 s`, `1.00x`). On `400k -> 500k` `perf stat`, `utxo-v4` executes `3.0%` fewer instructions, `6.8%` fewer LLC misses, and `47.1%` fewer dTLB misses than `pr-201` while holding `13.50` active CPUs (vs `12.85`).
- Virtually the entire remaining `+35.7 s` wall-time delta sits in the single-threaded final shutdown flush (`79.9 s` vs `45.5 s`, `+34.4 s`).
- Benchmark caveat: during `-reindex-chainstate` (where `rev*.dat` already exists on disk), `utxo-v4` sets `build_undo = false` to skip constructing a throwaway `CBlockUndo`, whereas `pr-201` still constructs `CBlockUndo` for its cold-path rollback. Real IBD from peers builds, serializes, and writes full undo data on both branches.

---

## 5. Deliberate omissions and follow-up optimization roadmap

Every remaining gap (the `+34.4 s` shutdown flush, `+1.2 GiB` RSS delta, optional cooperative commit helping on extreme core counts, and small-cache / live-network improvements) is isolated behind existing module boundaries and can be landed independently without changing the core architecture.

### 5.1 Cooperative commit helping on very high core counts

- **Current behavior:** Worker pool expansion scales up to `MAX_PREVOUTFETCH_THREADS = 256` and `ComputeIBDWindowWidth()` scales up to `512` blocks with a dynamic `WindowOutputTable` ring buffer, while `ShardedCoinsView::CommitBlockBatch()` uses `SubmitPriority()` helper tasks rather than polling mid-block inside `ValidateBlockWithFlatInputs()`.
- **Follow-up path:** Keep `ValidateBlockWithFlatInputs()` decoupled from `ShardedCoinsView` in the base series; add a `TryHelpCommit()` hook only on a follow-up branch if 64 to 256-core profiles show commit barrier stalls.

### 5.2 Eliminate per-operation `ShardUndoEntry` logging on the happy commit path

- **Why `utxo-v4` spends ~4.6% of cycles here:** `ApplyBatchSpend()` and `ApplyBatchCreate()` construct and push a ~96-byte `ShardUndoEntry` (moving the `StoredCoin` out of `LiveEntry`) for every single spend (~1.47B) and create (~1.63B), then destroy them on the next `undo.clear()`. On valid mainnet chains, rollback is never triggered.
- **What `pr-201` does:** Mutates shards in place without recording a rollback entry per operation, recording only rare overwritten coins (`possible_overwrite`) and replaying `CBlockUndo` on the cold conflict path (`TakeBackCommit()`).
- **Follow-up path:** Replace `m_commit_undos` with cold-path `CBlockUndo` + created-outpoint reversal when `committed < initial_limit` (self-contained in `src/coins_sharded.cpp` and `src/validation.cpp`; note `-reindex-chainstate` skips building `CBlockUndo` in memory, so rollback would need to read or reconstruct spent coins on the cold conflict path).

### 5.3 Multi-threaded LevelDB final flush (`~34 s` wall time)

- **Why `utxo-v4` is slower at shutdown:** `ShardedCoinsView::Flush()` sorts all 256 shards in parallel (`~7.4 s`), then encodes and streams all 165.5M entries into `CCoinsViewDB::BatchWriteEntries()` on a single thread (`66.8 s`, ~167 MiB/s). Because each ~33 MiB LevelDB memtable flushes in ~80 ms while the single writer takes ~197 ms to produce the next one, LevelDB's background thread uses the ~117 ms gaps (`imm_ == nullptr`) to run 84 L1 -> L2 -> L3 compactions, writing `19.4 GiB` (`files[ 0 43 203 81 0 0 0 ]`).
- **What `pr-201` does:** Keeps `DB_BEST_BLOCK`/`DB_HEAD_BLOCKS` open once via `m_open_marker` and runs up to 4 concurrent LevelDB writer workers over disjoint contiguous shard ranges (`MAX_WIPE_WORKERS = 4`), saturating `CompactMemTable()` so LevelDB only runs 16 compactions and writes `11.05 GiB` directly into L1 (`files[ 0 475 28 0 0 0 0 ]`, `45.5 s` total shutdown flush).
- **Follow-up path:** Add multi-worker disjoint-shard `WriteRun` support in `CCoinsViewDB` and `ShardedCoinsView::Flush()` (`src/txdb.{h,cpp}`, `src/coins_sharded.cpp`).

### 5.4 Stage 1 and Stage 3 per-block allocation, zero-init, and sorting overhead (estimated ~25 to 40 s)

- **Zero-initialized read buffer (`src/node/blockstorage.cpp`):** `std::vector<std::byte> data(blk_size)` zero-fills every >4 KiB block buffer (~700+ GB across mainnet) immediately before `pread()` overwrites every byte. Replacing it with `std::make_unique_for_overwrite<std::byte[]>(blk_size)` (or a reused thread-local buffer) eliminates this write pass in ~10 lines.
- **Intermediate Stage 3 vectors and `ext_refs` sort:** `RunValidateTask()` allocates per-block vectors (`ext_refs`, `unique_ops`, `unique_flat_idxs`, `window_hits`, `cache_ops`, `cache_to_unique`, `cache_results`), sorts all external inputs (`O(N log N)` per block), and hashes every outpoint twice (once in `LookupBatch()`, once in `CommitBlockBatch()`). Reusing `thread_local` scratch buffers, writing resolved `Coin`s directly into `resolved_inputs` in place, and caching `hash32` inside `BlockSpendOp` eliminates most of this per-block CPU and allocator churn.
- **Dedicated block I/O thread:** `pr-201` offloads blocking disk reads (`ReadBlockBytes()`) onto a dedicated I/O thread so compute workers never sleep on NVMe page faults. Our `posix_fadvise(WILLNEED)` coalescing already hides most sequential latency, so a separate I/O thread is optional and most useful on slower SATA/HDD storage.

### 5.5 Cache table indirection and memory footprint (`~20 s` + `~1.6 GiB` RSS)

- **Why `utxo-v4` uses more DRAM hops and RSS:** `ShardedCoinsView` uses a two-level indirection (`IndexSlot` table -> slab `LiveEntry`, two random cache-line fetches per probe) plus `dirty_live_slots` (`vector<uint32_t>`, ~0.7 GiB at 172M UTXOs) and `m_commit_undos` scratch buffers.
- **What `pr-201` does:** Stores 88/92-byte cells in 32 MiB aligned `mmap` regions linked by 32-bit cell indices with shard-wide `PrefetchAll()` before probing, and tracks dirty entries via per-1024-height byte-sum spans rather than per-entry dirty vectors (`19.7 GiB` peak RSS vs `21.3 GiB`).
- **Follow-up path:** Prefetching `IndexSlot` + `LiveEntry` across whole shard batches in `LookupBatch()` and removing `dirty_live_slots` (scanning occupied slab entries only during `Flush()`/`Sync()`, or tracking dirty bitsets per 32,768-entry slab: 4 KiB per slab = ~21 MiB total instead of ~700 MiB) can recover most of the RSS and latency gap without replacing the slab allocator.

### 5.6 Small-`-dbcache` streaming lag runs and live-network P2P receive pool

Not exercised by `-dbcache=32000 -networkactive=0`, but relevant for constrained-RAM and high-bandwidth live IBD:

- **Partial lagging write-outs (`CoinsStore` runs in `pr-201`):** At `-dbcache=450` to `4096`, `utxo-v4` performs full `Flush()` / `Sync()` cycles when the cache fills, wiping hot UTXOs. Adding bounded-height partial write-out runs (flushing only older dirty heights and evicting clean entries with a clock sweep) would substantially improve small-`-dbcache` IBD without touching the window validation pipeline.
- **Parallel P2P block receive pool + adaptive per-peer download rate (`pr-201` `m_receive_pool`):** During live P2P IBD on >500 Mbit/s links, `utxo-v4` deserializes and hashes incoming wire blocks on the single `msghand` thread (off `cs_main`). Adding a bounded receive pool (4 to 5 threads) feeding `StoreNewBlock()` + `BlockHandoffCache` cleanly extends the existing `ibdconnect` handoff boundary.
