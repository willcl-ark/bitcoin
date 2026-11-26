# UTXO v2 Series Architecture, Rationale, and Future Optimizations (Temporary)

> **Temporary document.** This file records the architectural choices, durability invariants, and deferred follow-up optimizations of the `utxo-v2` stack for reviewers and agents. Drop this commit before upstream submission.

## Goals and target environments

The `utxo-v2` stack accelerates initial block download (IBD) and reindexing across all Bitcoin Core host classes, including low-RAM HDD/SD nodes (2 to 4 GiB RAM), desktop NVMe machines (8 to 16 GiB RAM), and multi-core servers (32+ GiB RAM), across Linux, macOS, and Windows.

Design constraints:

- Preserve consensus behavior and crash/power-loss durability guarantees.
- Keep steady-state post-IBD thread count, memory usage, and P2P behavior aligned with upstream `master`.
- Prefer small, single-purpose components over generic abstractions.

## Stack structure

The series sits cleanly in four layers on top of upstream `master` (`utxo-split/base`):

1. **`utxo-v2/1-independent-wins` (8 commits).** Standalone wins that can go upstream independently (unzeroed `ReadBlock` buffer, narrowed compaction error catch, capped prevout prefetch tasks, explicit file-sync tracking, `WAITED_FOR_UNDER_LOCK` lock-order checks, `GETBLOCKTXN` read-error disconnect, single-pass transaction hashing from stream bytes, and cross-platform micro-wins for `PoolResource`, `SignatureCache`, `BufferedWriter`, and `FlatFileSeq::Open`).
2. **`utxo-v2/2-preparatory-refactor` (4 commits).** Script execution cache lock with empty fast path, pure `ConnectBlock`/`DisconnectBlock` over `BlockValidationContext` with explicit `std::span<const Coin>` overloads, off-`cs_main` block checks and writes with privatized helpers, and virtualized `CCoinsViewCache` with sorted LevelDB batch writes.
3. **`utxo-v2/3-rearchitecture` (3 commits).** Packed dense sharded `CoinsStore` + `BlockApplyView`, focused single-pass async `UndoWriter` with persistent file handle and ordered prune unlinks, and 64-shard `WindowOutputs` + parallel `ConnectWindow` sharing `m_connect_pool`.
4. **`utxo-v2/4-async-p2p-handoff` (1 commit).** IBD-only asynchronous P2P block receive (`blockrecv`, lazy-started) and background chain activation (`BlockConnector` / `blockconn`, lazy-started) with non-blocking handoff, non-blocking deferred `PONG`, connector wakeups, and prune-mode download lookahead caps.

## Core design decisions and rationale

### Packed dense sharded `CoinsStore`

Files: `src/coinsstore.h`, `src/coinsstore.cpp`

`CoinsStore` replaces a single `std::unordered_map` + `PoolResource` with 128 independent shards. Each shard stores entries in a dense `std::vector<PackedEntry>` paired with a power-of-2 `std::vector<uint32_t>` open-addressing hash table using backward-shift deletion and O(1) swap-with-last removal.

Each `PackedEntry` is 88 bytes (36-byte `COutPoint`, 4-byte packed height and coinbase/dirty/fresh bitfield, 8-byte `CAmount` with `-1` as the spent tombstone sentinel, and a 40-byte `CScript` with inline `prevector` storage for standard scripts). Together with the 4-byte index slots at <= 70% load factor, storage averages ~94 bytes per UTXO uniformly across libstdc++, libc++, and MSVC (~16.9 GiB for 180M UTXOs, down from 23 to 27 GiB with `std::unordered_map`).

Key choices:

- **Zero allocation on empty shards.** Empty shards allocate nothing, removing the ~4 MiB floor that `PoolResource` imposed on small `-dbcache` setups (such as `-dbcache=4`).
- **Relaxed per-shard atomic counters.** Each shard publishes its byte usage, entry count, and dirty count once per lock acquisition instead of contending on a shared atomic counter array.
- **O(shards) `Flush()`.** After writing dirty entries in sorted order to LevelDB, `Flush()` clears each shard's dense vector and index table in one step without rescanning entries.
- **Flush generation (`m_flush_gen`).** Incremented on every `Flush()` and `Sync()`, enabling `FROM_BASE` verification during batched commits without polluting `CoinsStore` with clean base reads.

### Single-lock batched shard commits and `FROM_BASE`

Files: `src/block_apply_view.h`, `src/block_apply_view.cpp`, `src/node/connect_window.h`, `src/node/connect_window.cpp`

During window validation, `BlockApplyView::ResolveInputs()` resolves every input of a block into a flat `std::vector<Coin>`:

1. Uncommitted outputs from lower window blocks and earlier transactions in the same block come from `WindowOutputs` (64 shards, sized down from 1024 to cut idle footprint).
2. Remaining inputs are fetched from `CoinsStore` via `FetchInto()`.
3. Cache misses fall back to `CoinsStore::PeekCoin()` (reading LevelDB without inserting clean entries into `CoinsStore`) and set a `from_base` flag.

Before resolving, `RunWindowValidation()` records `resolve_flush_gen = store.FlushGeneration()`. When `CommitGroup()` commits a batch of up to 32 ready blocks, it buckets all `CommitItem`s across `b = 0..count-1` by shard and calls `CoinsStore::ApplyShard()` once per shard.

When an item carries `FROM_BASE` and `s.resolve_flush_gen == job.store_flush_gen`, `ApplyShard()` knows a shard miss means the coin is still untouched in LevelDB (any intervening spend would have written a dirty tombstone, and any intervening flush or sync would have advanced `m_flush_gen`). This eliminates clean base-read cache pollution, post-flush double LevelDB reads, and per-block commit barriers while preserving exact conflict detection and `TakeBackCommit()` rollback.

### Explicit `std::span<const Coin>` consensus validation

Files: `src/consensus/tx_verify.h`, `src/consensus/tx_verify.cpp`, `src/validation.h`, `src/validation.cpp`

`CheckTxInputs()` and `GetTransactionSigOpCost()` provide `std::span<const Coin>` overloads sharing internal templates with the `CCoinsViewCache` overloads. Window validation passes resolved input coins explicitly through `BlockValidationContext::input_coins` (`flat_inputs = true`), keeping consensus checks direct and avoiding pointer-aliasing tricks on `CCoinsViewCache`.

### Thread pool consolidation and read throttling

Files: `src/validation.h`, `src/validation.cpp`, `src/node/blockstorage.h`, `src/node/blockstorage.cpp`

The consolidated threading model keeps thread counts proportional to configured cores and avoids idle post-IBD pools:

- **`m_script_check_queue`** (upstream) for serial script verification.
- **`m_connect_pool`** (`connect`, 2 to 16 threads scaled strictly from `worker_threads_num`, never inflated by `prevoutfetch_threads_num`). It serves window validation, parallel commit helpers, and serial `ConnectTip` prevout prefetching (no separate `m_prevout_pool`).
- **`m_receive_pool`** (`blockrecv`, 2 to 4 threads, started lazily via `std::call_once` on the first IBD P2P handoff). Kept separate from `m_connect_pool` to prevent lock-order deadlocks because receive tasks acquire `cs_main` while `cs_main` waits on `m_connect_pool` during commits and drains.
- **Direct reads with `std::counting_semaphore<4>`.** `RunConnectTask()` reads blocks directly on `m_connect_pool` under a 4-permit semaphore rather than bouncing through a dedicated block I/O thread pool and per-block `std::promise`. This removes 4 threads, eliminates a thread hop and two heap allocations per block, and bounds concurrent disk seeks on spinning drives.
- **One ordered `UndoWriter` thread (`undowriter`)** inside `BlockManager`, handling both undo writes and async pruned-file unlinks.
- **One lazy `BlockConnector` thread (`blockconn`)**, started only on the first IBD handoff.

On a synced node restarting out of IBD (or in single-threaded `libbitcoinkernel`), `blockrecv` and `blockconn` spawn zero threads.

### Single-pass undo serialization and persistent-handle `UndoWriter`

Files: `src/node/blockstorage.h`, `src/node/blockstorage.cpp`, `src/validation.cpp`

A focused undo writer built directly into `BlockManager` avoids generic effect frameworks and 10 ms `TRY_LOCK(cs_main)` polling:

- **Single-pass parallel serialization.** `BlockManager::SerializeBlockUndo()` serializes `MessageStart || size || CBlockUndo || SHA256(prev_hash || undo)` in one pass instead of three. During window IBD this runs in parallel on `m_connect_pool` workers, shrinking undo backlog RAM and turning the writer thread into a pure I/O loop.
- **Persistent `AutoFile` handle.** While back-to-back writes target the same `revNNNNN.dat`, `UndoWriterLoop()` keeps the `AutoFile` open, writes via `write_buffer()` (in-place XOR + one `fwrite`), and calls `fflush()` so concurrent readers immediately see written bytes in the OS page cache. The handle is closed before switching files, before chunk preallocation, before pruned-file unlinks, on errors, and whenever the queue drains (so `SettleUndoWrites()`, flushes, trims, and idle periods hold no open undo descriptor).
- **No fsync under `cs_main` on file rotation.** Undo file completion calls `TrimUndoFile(*finishes)` rather than `FlushUndoFile(*finishes, true)`, avoiding macOS `F_FULLFSYNC` and HDD fsync stalls under `cs_main`. Durability is enforced by `SyncWrittenFiles()` before `WriteBlockIndexDB()`.
- **Ordered prune unlinks.** `FlushStateToDisk()` routes async unlinks through `EnqueueUnlinkPrunedFiles()` on the same thread, guaranteeing unlinks execute strictly after earlier undo writes and handle closes (required on Windows).
- **Deterministic completion settling.** `ActivateBestChain()` always settles pending undo completions on every normal exit path.

### Crash and power-loss durability invariants

Files: `src/node/blockstorage.cpp`, `src/validation.cpp`

Every flush in `FlushStateToDisk()` enforces the strict order:

1. `SettleUndoWrites()` drains all queued undo I/O, closes the active undo handle, and applies completions under `cs_main`.
2. `FlushChainstateBlockFile()` and `SyncWrittenFiles()` fsync every file in `m_unsynced_block_files` and `m_unsynced_undo_files`.
3. `WriteBlockIndexDB()` persists block index and file metadata synchronously.
4. `EnqueueUnlinkPrunedFiles()` (or synchronous `UnlinkPrunedFiles()` for manual `pruneblockchain`) removes pruned files only after the index records the prune.
5. `CoinsTip().Flush()` or `CoinsTip().Sync()` persists the UTXO set.

Additional safeguards:

- Off-lock writes re-insert their file numbers into `m_unsynced_block_files` (`UnpinBlockFile`) and `m_unsynced_undo_files` (undo completion) before marking `BLOCK_HAVE_DATA`/`BLOCK_HAVE_UNDO`, preventing a concurrent `FlushChainstateBlockFile()` from erasing a file before an in-flight write lands.
- Any block/undo sync or write error latches permanently (`m_sync_failed`, `m_undo_write_failed`) and blocks further index or UTXO flushes.
- The pre-index low-disk-space retry calls `ScanAndUnlinkAlreadyPrunedFiles(setFilesToPrune)` so files pruned in the current flush are never unlinked before `WriteBlockIndexDB()`.

### IBD-only async P2P handoff

Files: `src/net_processing.cpp`, `src/node/block_connector.h`, `src/node/block_connector.cpp`

- **IBD-only activation.** `ProcessBlock()` only passes `activate_async = true` while `m_chainman.IsInitialBlockDownload()` is true (and `!m_opts.blocks_on_message_handler`). On the first post-IBD entry into `ProcessMessages()`, a one-shot `WaitForPendingBlocks()` drains any remaining IBD tasks, after which all P2P block processing runs synchronously on `msghand` just like upstream `master`.
- **Non-blocking `HandOffBlock()`.** If `m_block_tasks` is already at capacity (`recv_workers * 4`), `HandOffBlock()` returns `false` immediately so `msghand` parses and stores the block inline instead of blocking `g_msgproc_mutex` on a condition variable.
- **Non-blocking deferred `PONG` and connector wakeups.** Deferred pings are stored in `Peer::m_pending_pongs` and flushed from both `ProcessMessages()` and `SendMessages()` without blocking `node.PollMessage()`. `BlockConnector` invokes `m_connman.WakeMessageHandler()` after each active activation pass so `msghand` flushes pending `PONG`s and refills block downloads immediately.
- **Prune-mode download lookahead cap.** In `FindNextBlocksToDownload()`, once `CalculateCurrentUsage() >= GetPruneTarget()` (for automatic pruning), `download_window` is capped to `clamp(ConnectWindow() * 2, 16, 64)` blocks ahead of `ActiveHeight()`, preventing buffered unvalidated blocks from blowing past `-prune=550`.

### Cross-platform micro-optimizations

- **Lazy `PoolResource` chunk allocation (`src/support/allocators/pool.h`).** Defers the first chunk allocation until the first `Allocate()` call.
- **Lock-free empty fast path for sigcache and script cache (`src/script/sigcache.h/.cpp`, `src/validation.h`).** A relaxed/acquire-release `m_empty` atomic avoids acquiring a `shared_lock` on every lookup while the caches are empty during `-assumevalid` IBD.
- **Uninitialized `BufferedWriter` buffer (`src/streams.h`).** Uses `std::make_unique_for_overwrite<std::byte[]>(size)` instead of zero-filling 64 KiB on every buffered file write.
- **Lazy directory creation in `FlatFileSeq::Open()` (`src/flatfile.cpp`).** Calls `fs::create_directories()` only when `fopen()` fails on a writable open.
- **Memory-bounded window accounts (`src/node/connect_window.h`, `src/validation.cpp`).** Window and pinned-block byte limits clamp to `clamp(coinstip_cache / 16, 16 MiB, 256 MiB)` so low-RAM nodes do not over-buffer blocks.

## Omitted optimizations and future follow-ups

The following optimizations were deliberately left out of `utxo-v2` to keep the diff reviewable and avoid adding complexity before measuring real-world Benchcoin profiles. Each is a well-scoped follow-up once this foundation lands.

### 1. Sub-block script parallelism for `-assumevalid=0` and post-assumevalid IBD

`RunWindowValidation()` passes `check_queue = nullptr` to `ConnectBlock()`, so scripts within a single block execute sequentially on one `m_connect_pool` worker while different blocks run in parallel across workers. When `ConnectWindow >= cores` and blocks have similar sizes, cores stay saturated. However, on `-assumevalid=0` or during the post-assumevalid tail when a single large block dominates the window (or when `ConnectWindow` is small), sharing `m_script_check_queue` across window workers (or switching to a unified work-stealing task pool) would reduce tail latency and improve scaling on high-core-count machines.

### 2. Compressed scripts / slab storage in `CoinsStore`

`CoinsStore::PackedEntry` is 88 bytes today because `CScript` (`prevector<28, unsigned char>`) occupies 40 bytes inline per entry even when standard P2PKH/P2SH/P2WPKH/P2WSH/P2TR scripts compress to 21 to 33 bytes (as `ScriptCompression` already does on disk). Storing compressed script tags or packing variable-length script payloads into a per-shard bump/slab arena could reduce `CoinsStore` memory by another 20 to 30% (~65 to 72 B/UTXO, saving 3.5 to 5 GiB on a 180M UTXO set) and dramatically cut `-dbcache` flushes on 8 to 16 GiB nodes.

### 3. Batched / pipelined LevelDB reads on cache misses

On low-RAM nodes (4 to 8 GiB `-dbcache`) where `CoinsStore` flushes frequently, `BlockApplyView::ResolveInputs()` falls back to per-key `CoinsStore::PeekCoin()` calls into LevelDB (`DB::Get()`). Sorting cache-miss outpoints per block (or per batch of blocks) before querying LevelDB, or adding a LevelDB `MultiGet`-style sorted lookup / prefetch pass, would improve L0/SST block-cache locality and reduce per-lookup mutex and comparator overhead in LevelDB.

### 4. Non-blocking / double-buffered `FlushStateToDisk()`

`CoinsStore::Flush()` and `Sync()` currently hold `cs_main` while sorting dirty entries and writing LevelDB batches. On multi-gigabyte `-dbcache` setups, periodic and critical flushes stall all block connection for seconds to tens of seconds. Copy-on-write or generation-tagged shard snapshots (moving dirty entries into an immutable flush view while new blocks continue mutating a fresh delta layer) would overlap LevelDB flushes with block validation.

### 5. Parallel radix sort or streaming SST writes for `CoinsStore::Flush()`

`CoinsStore::Flush()` sorts entries per shard and merges them in key order before writing to LevelDB. On large `-dbcache` builds (16 to 32 GiB), sorting tens of millions of `COutPoint` keys is a noticeable CPU spike during flush. Parallelizing per-shard sorting on `m_connect_pool` (with a compact `(key_prefix, shard, slot)` sort key) or ingesting pre-sorted SST files would cut flush pause times substantially.

### 6. Arena / slab allocation and deferred teardown for `CBlock` and `CTransaction`

Parsing and releasing millions of `CBlock` / `CTransaction` / `CTxIn` / `CTxOut` objects during IBD creates heavy allocator pressure and cross-thread free traffic between `blockrecv`, `connect`, and `msghand`. A previous prototype used a deferred-destruction block pool (`NewBlock(on_pool)`), which was removed here to simplify the stack. A cleaner follow-up is a contiguous per-block arena allocator for deserialized `CBlock` payloads (one allocation on parse, one free on release), which eliminates thousands of `malloc`/`free` calls per block without global pool complexity.

### 7. Passing received `CBlock` objects directly from `blockrecv` into `ConnectWindow`

During IBD, `blockrecv` deserializes a `CBlock`, validates stateless/contextual checks, writes it to `blkNNNNN.dat`, and drops the parsed `CBlock` (keeping only the latest block pointer for `BlockConnector`). `ConnectWindow` then re-reads the block bytes from disk (typically hitting the OS page cache) and deserializes it a second time. A small bounded in-memory handoff cache (keyed by block hash, capped to e.g. `min(window_limit / 2, 64 MiB)`) between `FinishNewBlock()` and `StageWindowBlock()` would skip redundant deserialization and syscall overhead whenever `ConnectWindow` keeps pace with P2P download.

### 8. Positional I/O (`pread`/`pwrite`) and readahead hints in `FlatFileSeq`

`BlockManager` still uses stdio `FILE*` with `fseek` + `fread`/`fwrite` + `fflush` for block reads/writes and undo writes. Moving hot block/undo reads and writes to direct positional OS calls (`pread`/`pwrite` on POSIX, `ReadFile`/`WriteFile` with `OVERLAPPED` offset on Windows) avoids stdio buffering overhead and mutexes, allows lock-free shared read descriptors across `m_connect_pool` workers, and pairs naturally with `posix_fadvise(POSIX_FADV_WILLNEED)` / `F_RDAHEAD` hints during sequential reindex and `ConnectWindow` reads.

### 9. Adaptive `ConnectWindow` width and disk-read concurrency

`m_block_read_sem` is currently fixed at 4 concurrent reads and `m_connect_window` scales linearly from `worker_threads_num`. On HDDs or SD cards, 1 to 2 sequential readers minimize head seeks, while fast NVMe drives benefit from 8+ concurrent reads and deeper windows. Measuring rolling read latency vs validation time and tuning read permits dynamically would improve both ends of the storage spectrum without manual flags.

### 10. Parallel `ConnectWindow` and async handoff for post-IBD assumeutxo background sync

`ConnectWindowStep` and `BlockConnector` currently gate on `m_chainman.IsInitialBlockDownload()`. Once an assumeutxo node's active tip catches up and exits IBD, the historical background chainstate falls back to serial `ActivateBestChainStep()`. Allowing the background chainstate to keep using `ConnectWindow` (while keeping active-tip P2P blocks synchronous on `msghand`) would speed up assumeutxo snapshot validation post-IBD.

### 11. Stopping IBD-only threads automatically after IBD exit

`m_receive_pool` (`blockrecv`) and `BlockConnector` (`blockconn`) are lazy-started, so an already-synced node restarting out of IBD spawns zero threads for them. However, a node that performs IBD and transitions into steady state keeps those parked threads alive until shutdown (in case an assumeutxo snapshot is loaded later via RPC). Stopping and joining `blockrecv` and `blockconn` cleanly once IBD ends and no historical chainstate is active (with re-start support if needed) would bring post-IBD thread count down to exact upstream parity even without a restart.
