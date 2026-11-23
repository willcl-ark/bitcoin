// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_COINSSTORE_H
#define BITCOIN_COINSSTORE_H

#include <coins.h>
#include <coins_entry_machine.h>
#include <crypto/common.h>
#include <kernel/caches.h>
#include <memusage.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <sync.h>
#include <uint256.h>
#include <undo.h>
#include <util/check.h>
#include <util/hasher.h>

#include <array>
#include <atomic>
#include <bit>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

class ThreadPool;

/**
 * The young tier of the UTXO set: the in-memory layer above the coins database
 * holding confirmed coins changed since the last write-out and clean copies of
 * old coins brought in by lookups.
 *
 *  - Sharded by leading bytes of txid (contiguous ranges of database keys) with
 *    one mutex per shard (default 512 shards).
 *  - Each shard stores its entries in a flat open-addressed SwissTable-style
 *    hash table (1-byte control tags + compact 88-byte inline Slot, allocated
 *    on mmap + MADV_HUGEPAGE for large shards and reclaimed via munmap on
 *    rehash/shrink/Flush/Reset/destruction). No node allocator, no per-entry
 *    linked-list pointers, and no background preallocator thread.
 *  - Holds confirmed state only: blocks in the connect window validate read-only
 *    against CoinsStore plus a tiny WindowOverlay holding the window's txs, and
 *    validated prefixes are checked and committed in exact height order across
 *    shards in parallel.
 *  - Every entry carries the height of its last change (or an earlier owed
 *    change height) so lagged write-out runs (BeginWriteOut / StreamWriteOut /
 *    CommitWriteOut) persist the oldest changes without emptying the cache.
 *  - Clean entries can be evicted independently via a linear clock hand
 *    (EvictClean()).
 */
class StoredCoinData
{
    friend class StoredCoin;

    unsigned char m_value[sizeof(CAmount)]{0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    CAmount Value() const
    {
        CAmount value;
        std::memcpy(&value, m_value, sizeof(value));
        return value;
    }
    void SetValue(CAmount value) { std::memcpy(m_value, &value, sizeof(value)); }

public:
    static constexpr size_t INLINE{35};

    Coin ToCoin() const
    {
        Coin coin;
        if (IsSpent()) return coin;
        coin.out.nValue = Value();
        const unsigned char* bytes{Script()};
        coin.out.scriptPubKey.assign(bytes, bytes + ScriptSize());
        coin.nHeight = nHeight;
        coin.fCoinBase = fCoinBase;
        return coin;
    }
    operator Coin() const { return ToCoin(); }

    bool IsSpent() const { return Value() == -1; }
    bool OnHeap() const { return m_size == HEAP; }
    size_t DynamicMemoryUsage() const { return m_size == HEAP ? memusage::MallocUsage(sizeof(uint32_t) + ScriptSize()) : 0; }

    uint32_t nHeight : 31 {0};
    uint32_t fCoinBase : 1 {0};

private:
    static constexpr uint8_t HEAP{0xff};
    static_assert(INLINE < HEAP && INLINE >= sizeof(unsigned char*));

    uint8_t m_size{0};
    unsigned char m_bytes[INLINE]{};

    unsigned char* HeapBlock() const
    {
        unsigned char* block;
        std::memcpy(&block, m_bytes, sizeof(block));
        return block;
    }
    size_t ScriptSize() const
    {
        if (m_size != HEAP) return m_size;
        uint32_t size;
        std::memcpy(&size, HeapBlock(), sizeof(size));
        return size;
    }
    const unsigned char* Script() const { return m_size == HEAP ? HeapBlock() + sizeof(uint32_t) : m_bytes; }
};
static_assert(std::is_trivially_copyable_v<StoredCoinData> && std::is_trivially_destructible_v<StoredCoinData>);

class StoredCoin : public StoredCoinData
{
public:
    StoredCoin() = default;
    StoredCoin(const Coin& coin) { Assign(coin); }
    StoredCoin(const StoredCoin& other) : StoredCoinData{} { CopyFrom(other); }
    explicit StoredCoin(const StoredCoinData& other) { CopyFrom(other); }
    StoredCoin(StoredCoin&& other) noexcept { MoveFrom(other); }
    StoredCoin& operator=(const Coin& coin)
    {
        Release();
        Assign(coin);
        return *this;
    }
    StoredCoin& operator=(const StoredCoin& other)
    {
        if (this != &other) {
            Release();
            CopyFrom(other);
        }
        return *this;
    }
    StoredCoin& operator=(StoredCoin&& other) noexcept
    {
        if (this != &other) {
            Release();
            MoveFrom(other);
        }
        return *this;
    }
    ~StoredCoin() { Release(); }

    void Clear()
    {
        Release();
        SetValue(-1);
        nHeight = 0;
        fCoinBase = false;
    }

private:
    void SetScript(const unsigned char* bytes, size_t size)
    {
        if (size <= INLINE) {
            m_size = static_cast<uint8_t>(size);
            if (size) std::memcpy(m_bytes, bytes, size);
            return;
        }
        auto* block{new unsigned char[sizeof(uint32_t) + size]};
        const uint32_t size32{static_cast<uint32_t>(size)};
        std::memcpy(block, &size32, sizeof(size32));
        std::memcpy(block + sizeof(uint32_t), bytes, size);
        std::memcpy(m_bytes, &block, sizeof(block));
        m_size = HEAP;
    }
    void Assign(const Coin& coin)
    {
        SetValue(coin.out.nValue);
        nHeight = coin.nHeight;
        fCoinBase = coin.fCoinBase;
        const CScript& script{coin.out.scriptPubKey};
        SetScript(script.data(), script.size());
    }
    void CopyFrom(const StoredCoinData& other)
    {
        std::memcpy(m_value, other.m_value, sizeof(m_value));
        nHeight = other.nHeight;
        fCoinBase = other.fCoinBase;
        SetScript(other.Script(), other.ScriptSize());
    }
    void MoveFrom(StoredCoin& other) noexcept
    {
        std::memcpy(m_value, other.m_value, sizeof(m_value));
        nHeight = other.nHeight;
        fCoinBase = other.fCoinBase;
        m_size = other.m_size;
        const size_t copy_len{other.m_size == HEAP ? sizeof(unsigned char*) : other.m_size};
        if (copy_len) std::memcpy(m_bytes, other.m_bytes, copy_len);
        other.m_size = 0;
        other.nHeight = 0;
        other.fCoinBase = false;
        other.SetValue(-1);
    }
    void Release() noexcept
    {
        if (m_size == HEAP) delete[] HeapBlock();
        m_size = 0;
    }
};
static_assert(sizeof(StoredCoin) == 48 && alignof(StoredCoin) == 4);

/**
 * Tiny 64-shard read-only overlay of transactions created by blocks currently in
 * the connect window (Stage 0 -> Stage 1). Keyed by Txid (2.5-3x fewer entries
 * than outpoints) in flat open-addressed per-shard tables pointing directly into
 * pinned shared_ptr<const CBlock> instances (zero coin copies, zero per-entry
 * heap allocations). Intra-block spends are excluded via a 64-bit spent mask
 * (plus a rare overflow vector for vout >= 64) so outputs created and spent in
 * the same block never leak to higher window blocks.
 */
class WindowOverlay
{
public:
    static constexpr size_t NUM_SHARDS{64};

    WindowOverlay() = default;
    WindowOverlay(const WindowOverlay&) = delete;
    WindowOverlay& operator=(const WindowOverlay&) = delete;
    ~WindowOverlay();

    //! Index all transactions of `block` (marking intra-block spent outputs as invisible).
    void AddBlockOutputs(int height, std::shared_ptr<const CBlock> block);
    //! Advance the committed-height watermark so readers ignore entries with height <= committed_height.
    void SetCommittedHeight(int height) noexcept { m_committed_height.store(height, std::memory_order_release); }
    //! Remove transactions of committed (or drained) blocks and release their block references.
    void RemoveBlocks(std::span<const int> heights, ThreadPool* pool = nullptr);
    //! Detach committed blocks immediately and run shard table removal asynchronously on `pool` (or inline if null/small).
    void RemoveBlocksAsync(std::span<const int> heights, ThreadPool* pool);
    //! Wait for any asynchronous RemoveBlocksAsync tasks to complete.
    void WaitForRemovals();
    //! Clear all blocks and entries (used when draining a window step).
    void Clear();

    //! Look up the highest output of `outpoint` created strictly below `viewer_height`.
    std::optional<Coin> Lookup(int viewer_height, const COutPoint& outpoint) const;

    //! Batch-fetch outputs created strictly below `viewer_height`. Hits are appended to `found`;
    //! misses are appended to `misses` for CoinsStore::FetchConfirmed().
    void Fetch(int viewer_height, std::span<const COutPoint> outpoints,
               std::vector<std::pair<COutPoint, Coin>>& found,
               std::vector<COutPoint>& misses) const;

private:
    struct TxEntry {
        const CTransaction* tx{nullptr};
        uint64_t intra_spent_mask{0};
        uint64_t hash{0};
        int32_t height{0};
        bool is_coinbase{false};
        bool has_high_intra_spend{false};
    };
    struct FlatTable {
        static constexpr size_t NOT_FOUND{std::numeric_limits<size_t>::max()};
        static constexpr uint8_t EMPTY{0};
        static constexpr uint8_t TOMBSTONE{1};
        static uint8_t Tag(uint64_t h) noexcept { return static_cast<uint8_t>((h >> 56) | 2); }

        std::vector<uint8_t> ctrl;
        std::vector<TxEntry> slots;
        size_t size{0};
        size_t tombstones{0};

        bool empty() const noexcept { return size == 0; }
        void clear() noexcept
        {
            ctrl.clear();
            slots.clear();
            size = 0;
            tombstones = 0;
        }
        void Rehash(size_t new_cap);
        size_t FindIndex(const Txid& txid, uint64_t h) const noexcept;
        TxEntry* Find(const Txid& txid, uint64_t h) noexcept;
        const TxEntry* Find(const Txid& txid, uint64_t h) const noexcept;
        TxEntry* TryInsert(const TxEntry& entry);
        void EraseAt(size_t idx) noexcept;
        bool Erase(const Txid& txid, uint64_t h, int32_t height) noexcept;
    };
    struct alignas(64) Shard {
        mutable Mutex mutex;
        FlatTable txs GUARDED_BY(mutex);
        std::vector<TxEntry> dup_txs GUARDED_BY(mutex);
        std::vector<std::pair<COutPoint, int32_t>> high_intra_spends GUARDED_BY(mutex);
    };
    struct RemItem {
        const Txid* txid;
        uint64_t hash;
    };
    struct BlockMeta {
        std::shared_ptr<const CBlock> block;
        std::vector<RemItem> items;
        std::array<uint32_t, NUM_SHARDS + 1> starts{};
    };

    struct FastTxidHash {
        uint64_t s0, s1, s2, s3, mul;
        FastTxidHash()
        {
            SaltedCoinsCacheHasher h;
            s0 = h(Txid::FromUint256(uint256{1}));
            s1 = h(Txid::FromUint256(uint256{2}));
            s2 = h(Txid::FromUint256(uint256{3}));
            s3 = h(Txid::FromUint256(uint256{4}));
            mul = h(Txid::FromUint256(uint256{5})) | 1ULL;
        }
        uint64_t operator()(const Txid& txid) const noexcept
        {
            const unsigned char* d{txid.ToUint256().data()};
            uint64_t x{(ReadLE64(d) ^ s0) + (ReadLE64(d + 8) ^ s1)};
            x ^= (ReadLE64(d + 16) ^ s2) + (ReadLE64(d + 24) ^ s3);
            x *= mul;
            return x ^ (x >> 32);
        }
    };
    static uint64_t TxHash(const Txid& txid) noexcept
    {
        static const FastTxidHash hasher;
        return hasher(txid);
    }
    static size_t ShardIndex(const Txid& txid) noexcept
    {
        return (uint64_t{ReadBE32(txid.ToUint256().data())} * NUM_SHARDS) >> 32;
    }
    static std::optional<Coin> ExtractCoin(const Shard& shard, const TxEntry& entry, uint32_t n) EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);
    void EraseDetached(std::vector<std::pair<int, BlockMeta>>& removing, ThreadPool* pool);

    std::array<Shard, NUM_SHARDS> m_shards;
    std::atomic<size_t> m_total_blocks{0};
    std::atomic<int> m_committed_height{-1};
    Mutex m_blocks_mutex;
    std::unordered_map<int, BlockMeta> m_blocks GUARDED_BY(m_blocks_mutex);
    Mutex m_remove_mutex;
    std::condition_variable m_remove_cv;
    int m_pending_removes GUARDED_BY(m_remove_mutex){0};
};

class CoinsStore : public CCoinsViewCache
{
public:
    //! One confirmed change to apply in CommitBatch()/CommitWindowBatch().
    struct CommitOp {
        COutPoint outpoint;
        Coin coin;
        uint64_t key_hash{0};
        uint32_t shard{0};
        int32_t height{0};
        int32_t spent_height{-1};
        bool possible_overwrite{false};
        bool cancelled{false};
    };

    //! Per-block net dirty UTXO changes pre-bucketed by shard on the Stage 1 worker thread.
    struct BlockChanges {
        std::vector<CommitOp> ops;
        std::vector<uint32_t> shard_offsets;
        int height{0};
        uint64_t validate_epoch{0};
        bool must_be_new{false};
    };

    //! Result of CommitWindowBatch(): contiguous conflict-free prefix length and conflicts on the stopping block.
    struct BatchCheckResult {
        size_t accepted{0};
        size_t conflicts{0};
    };

    //! A write-out run between BeginWriteOut() and CommitWriteOut().
    struct WriteOutRun {
        std::vector<CoinWrite> restored;
        size_t count{0};
        size_t chunks{0};
        uint256 new_head;
        uint256 old_head;
        int bound_height{-1};
        int head_height{-1};
        int oldest_left{std::numeric_limits<int>::max()};
        int settled_height{-1};
        uint256 settled_head;
        bool final{false};
        size_t bytes{0};
    };

    static constexpr size_t DEFAULT_SHARDS{256};
    static constexpr size_t COMMIT_OPS_PER_HELPER{256};

    explicit CoinsStore(CCoinsView* base_view, size_t shards = DEFAULT_SHARDS, bool deterministic = false);
    ~CoinsStore() override;

    // CCoinsView: confirmed state.
    std::optional<Coin> GetCoin(const COutPoint& outpoint) const override;
    std::optional<Coin> PeekCoin(const COutPoint& outpoint) const override;
    bool HaveCoin(const COutPoint& outpoint) const override;
    uint256 GetBestBlock() const override;
    void BatchWrite(CoinsViewCacheCursor& cursor, const uint256& block_hash) override;

    // CCoinsViewCache: confirmed mutations.
    void SetBestBlock(const uint256& block_hash) override;
    bool HaveCoinInCache(const COutPoint& outpoint) const override;
    const Coin& AccessCoin(const COutPoint& outpoint) const override;
    void AddCoin(const COutPoint& outpoint, Coin&& coin, bool possible_overwrite) override;
    void EmplaceCoinInternalDANGER(const COutPoint& outpoint, Coin&& coin) override;
    bool SpendCoin(const COutPoint& outpoint, Coin* moveto = nullptr) override;
    void Flush(bool reallocate_cache = true) override;
    void Sync();
    void SetBaseWrites(uint64_t batch_bytes, bool concurrent)
    {
        m_batch_bytes = batch_bytes;
        m_base_concurrent = concurrent;
    }
    void SetFlushHelpers(std::shared_ptr<ThreadPool> pool) { m_flush_helpers = std::move(pool); }
    void Uncache(const COutPoint& outpoint) override;
    unsigned int GetCacheSize() const override;
    size_t GetDirtyCount() const noexcept override;
    size_t DynamicMemoryUsage() const override;
    void SanityCheck() const override;
    void Reset() noexcept override;

    // Tier heights & shard-parallel ordered commit.
    void SetBestBlock(const uint256& block_hash, int height);
    void SetChangeHeight(int height) { m_change_height = height; }
    int GetChangeHeight() const { return m_change_height; }
    void SetTierHeight(int height)
    {
        if (height < (m_run_in_flight ? std::max(m_written_height, m_inflight_settled_height) : m_written_height)) m_fell_below_written = true;
        m_tier_height = height;
        m_confirmed_height.store(height, std::memory_order_release);
    }
    int ConfirmedHeight() const { return m_confirmed_height.load(std::memory_order_acquire); }
    void PublishConfirmedHeight(int height) { m_confirmed_height.store(height, std::memory_order_release); }
    uint64_t WriteOutEpoch() const { return m_write_out_epoch.load(std::memory_order_acquire); }
    size_t NumShards() const { return m_shards.size(); }

    uint32_t ShardOf(const COutPoint& outpoint) const { return static_cast<uint32_t>(ShardIndex(outpoint)); }
    static uint64_t KeyHashOf(const COutPoint& outpoint) noexcept { return KeyHash(outpoint); }

    std::vector<std::pair<COutPoint, Coin>> FetchConfirmed(std::vector<COutPoint>&& outpoints) const;
    void FetchConfirmedInto(std::span<const COutPoint> outpoints, std::vector<std::pair<COutPoint, Coin>>& found) const;
    //! Fused single-pass conflict check + ordered commit across shards with exact per-block prefix rollback on conflict.
    BatchCheckResult CommitWindowBatch(std::span<BlockChanges*> blocks, std::span<const uint256> block_hashes, ThreadPool* pool = nullptr, bool publish_height = false);
    void CommitBatch(std::span<CommitOp> ops, const uint256& best_hash, int best_height, ThreadPool* pool = nullptr, bool publish_height = true);

    int BoundForBytes(size_t bytes, int tip_height) const;
    size_t EvictClean(size_t target_bytes);

    std::unique_ptr<WriteOutRun> BeginWriteOut(int bound_height, int head_height, const uint256& new_head, bool final);
    void StreamWriteOut(WriteOutRun& run, const std::function<uint256(int)>& block_at, uint64_t batch_bytes = DEFAULT_DB_CACHE_BATCH);
    void KeepWritten(std::vector<CoinWrite> restored);
    std::vector<CoinWrite> TakeRestored() { return std::exchange(m_restored, {}); }
    const std::vector<CoinWrite>& Restored() const { return m_restored; }
    void CommitWriteOut(const WriteOutRun& run);
    void AbandonWriteOut();
    const uint256& WrittenHead() const { return m_written_head; }
    int WrittenHeight() const { return m_written_height; }
    void InitWrittenHead(const uint256& block_hash, int height)
    {
        m_written_head = block_hash;
        m_written_height = height;
        SetTierHeight(height);
    }
    bool RunInFlight() const { return m_run_in_flight; }
    bool FlushFailed() const { return m_flush_failed; }

private:
    static constexpr size_t HUGE_PAGE_BYTES{size_t{2} << 20};
    static constexpr size_t NOT_FOUND{std::numeric_limits<size_t>::max()};

    struct Slot {
        COutPoint key;
        StoredCoin coin;
        int32_t height : 28 {0};
        uint32_t dirty : 1 {0};
        uint32_t fresh : 1 {0};
        uint32_t collected : 1 {0};
        uint32_t referenced : 1 {0};
    };
    static_assert(sizeof(Slot) == 88 && alignof(Slot) == 4);

    static uint64_t KeyHash(const COutPoint& outpoint) noexcept
    {
        struct FastOutpointHash {
            uint64_t s0, s1, s2, s3, mul;
            FastOutpointHash()
            {
                SaltedCoinsCacheHasher h;
                s0 = h(Txid::FromUint256(uint256{11}));
                s1 = h(Txid::FromUint256(uint256{12}));
                s2 = h(Txid::FromUint256(uint256{13}));
                s3 = h(Txid::FromUint256(uint256{14}));
                mul = h(Txid::FromUint256(uint256{15})) | 1ULL;
            }
            uint64_t operator()(const COutPoint& op) const noexcept
            {
                const unsigned char* d{op.hash.ToUint256().data()};
                uint64_t h{(ReadLE64(d) ^ s0) + (ReadLE64(d + 8) ^ s1)};
                h ^= (ReadLE64(d + 16) ^ s2) + (ReadLE64(d + 24) ^ s3);
                h ^= uint64_t{op.n} * 0x9e3779b97f4a7c15ULL;
                h *= mul;
                return h ^ (h >> 31);
            }
        };
        static const FastOutpointHash hasher;
        return hasher(outpoint);
    }

    /**
     * Flat open-addressed SwissTable-style per-shard hash table.
     * Single contiguous allocation (mmap >= 2 MiB with MADV_HUGEPAGE >= 8 MiB, operator new < 2 MiB) holding:
     *   [ctrl bytes (cap, 16-byte aligned groups)] [padding to 64B] [Slot[cap]]
     * Freed immediately via munmap on rehash/shrink/Clear/destruction.
     */
    class FlatMap
    {
    public:
        static constexpr size_t MIN_CAP{16};
        static constexpr uint8_t EMPTY{0x00};
        static constexpr uint8_t TOMBSTONE{0x01};
        static uint8_t Tag(uint64_t hash) noexcept { return static_cast<uint8_t>(0x80u | (hash >> 57)); }

        FlatMap() = default;
        FlatMap(const FlatMap&) = delete;
        FlatMap& operator=(const FlatMap&) = delete;
        ~FlatMap() { Clear(); }

        size_t size() const noexcept { return m_size; }
        size_t cap() const noexcept { return m_cap; }
        size_t tombstones() const noexcept { return m_tombstones; }
        uint64_t Gen() const noexcept { return m_gen; }
        size_t MemoryBytes() const noexcept { return m_region_bytes; }

        bool IsOccupiedAt(size_t idx) const noexcept { return (m_ctrl[idx] & 0x80u) != 0; }
        uint8_t CtrlAt(size_t idx) const noexcept { return m_ctrl[idx]; }
        Slot& SlotAt(size_t idx) noexcept { return m_slots[idx]; }
        const Slot& SlotAt(size_t idx) const noexcept { return m_slots[idx]; }

        size_t Find(const COutPoint& key, uint64_t hash) const noexcept;
        size_t Find(const COutPoint& key) const noexcept { return Find(key, KeyHash(key)); }

        bool NeedsGrow(size_t additional) const noexcept
        {
            if (additional == 0) return false;
            return m_cap == 0 || (m_size + m_tombstones + additional) * 8 >= m_cap * 7;
        }
        void EnsureCapacity(size_t additional);
        //! Find existing slot or insert a default-constructed Slot for `key`. Returns {idx, inserted}.
        std::pair<size_t, bool> TryEmplace(const COutPoint& key, uint64_t hash);
        std::pair<size_t, bool> TryEmplace(const COutPoint& key) { return TryEmplace(key, KeyHash(key)); }
        void EraseAt(size_t idx) noexcept;
        bool ShouldShrink() const noexcept
        {
            return (m_size == 0 && m_cap > 0) || (m_cap > MIN_CAP && m_size * 3 <= m_cap);
        }
        void ShrinkToFit();
        void Clear() noexcept;

    private:
        static size_t AlignUp64(size_t n) noexcept { return (n + 63) & ~size_t{63}; }
        void SetCtrl(size_t idx, uint8_t val) noexcept { m_ctrl[idx] = val; }
        void Rehash(size_t new_cap);
        static void* AllocRegion(size_t bytes, bool& mmapped);
        static void FreeRegion(void* ptr, size_t bytes, bool mmapped) noexcept;

        uint8_t* m_ctrl{nullptr};
        Slot* m_slots{nullptr};
        void* m_region{nullptr};
        size_t m_region_bytes{0};
        size_t m_cap{0};
        size_t m_size{0};
        size_t m_tombstones{0};
        uint64_t m_gen{0};
        bool m_mmapped{false};
    };

    struct RevertMeta {
        static constexpr uint32_t NO_COIN{std::numeric_limits<uint32_t>::max()};
        uint32_t block_idx;
        uint32_t op_idx;
        uint32_t coin_idx;
        int32_t old_height;
        uint8_t old_flags;
        bool existed;
    };

    using ShardMutex = Mutex;
    struct alignas(64) Shard {
        mutable ShardMutex mutex;
        FlatMap table GUARDED_BY(mutex);
        size_t dirty_count GUARDED_BY(mutex){0};
        size_t entries GUARDED_BY(mutex){0};
        size_t table_bytes GUARDED_BY(mutex){0};
        size_t heap_bytes GUARDED_BY(mutex){0};
        std::vector<int64_t> dirty_span_bytes GUARDED_BY(mutex);
        size_t clock_hand GUARDED_BY(mutex){0};
        uint64_t settles GUARDED_BY(mutex){0};
        uint64_t revert_batch_id GUARDED_BY(mutex){0};
        std::vector<RevertMeta> revert_meta GUARDED_BY(mutex);
        std::vector<StoredCoin> revert_coins GUARDED_BY(mutex);
    };
    uint64_t m_commit_batch_id{0};

    struct alignas(64) Totals {
        std::atomic<int64_t> bytes{0};
        std::atomic<int64_t> entries{0};
        std::atomic<int64_t> dirty{0};
    };
    static constexpr size_t TOTALS_STRIPES{64};
    Totals& MyTotals() const;
    size_t SumTotals(std::atomic<int64_t> Totals::* field) const;

    static constexpr int DIRTY_SPAN_BITS{10};
    static size_t SpanOf(int height);
    static int64_t DirtyBytes(const Slot& slot) noexcept;
    void AddDirtySpanBytes(Shard& shard, int height, int64_t delta) const EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);
    void AddUsage(Shard& shard, size_t bytes) const EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);
    void SubUsage(Shard& shard, size_t bytes) const EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);
    void SyncTableStats(Shard& shard) const EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);
    void SyncDirtyCount(Shard& shard, int64_t delta) const EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);

    Shard& ShardFor(const COutPoint& outpoint) const { return m_shards[ShardIndex(outpoint)]; }
    static void Reference(Slot& slot) noexcept { if (!slot.dirty && !slot.referenced) slot.referenced = 1; }

    using State = coins_entry::State;
    using Event = coins_entry::Event;
    static State StateOf(const Slot& slot) noexcept
    {
        using namespace coins_entry;
        return static_cast<State>((slot.dirty ? DIRTY : 0) | (slot.fresh ? FRESH : 0) |
                                  (slot.collected ? COLLECTED : 0) | (slot.coin.IsSpent() ? SPENT : 0));
    }
    static void Enter(Slot& slot, State to, std::optional<int> height)
    {
        using namespace coins_entry;
        Assert(to != REFUSED);
        Assert(static_cast<bool>(to & SPENT) == slot.coin.IsSpent());
        Assert(height.has_value());
        slot.height = *height;
        slot.dirty = (to & DIRTY) ? 1 : 0;
        slot.fresh = (to & FRESH) ? 1 : 0;
        slot.collected = (to & COLLECTED) ? 1 : 0;
    }
    static void Transit(Slot& slot, State from, Event event, int height)
    {
        using namespace coins_entry;
        Enter(slot, Next(from, event), NextHeight(HeightRule(from, event), from, int{slot.height}, height));
    }
    static void Transit(Slot& slot, State from, Event event)
    {
        Assert(coins_entry::HeightRule(from, event) == coins_entry::Height::Keep);
        Enter(slot, coins_entry::Next(from, event), int{slot.height});
    }
    static void Transit(Slot& slot, Event event, int height) { Transit(slot, StateOf(slot), event, height); }
    static void Transit(Slot& slot, Event event) { Transit(slot, StateOf(slot), event); }

    struct SettledRef {
        size_t pos;
        uint64_t hash;
        uint64_t gen;
        COutPoint key;
    };

    void CollectChunk(Shard& shard, size_t& cursor, WriteOutRun& run, uint64_t limit, uint64_t& size, std::vector<CoinWrite>& writes, std::vector<SettledRef>& settled) const EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);
    void SettleSlot(Shard& shard, size_t pos) const EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);
    void EraseSlot(Shard& shard, size_t pos) const EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);
    bool SpendSlot(Shard& shard, size_t pos, Coin* moveto) EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);
    std::optional<Coin> Lookup(const COutPoint& outpoint) const;

    template <typename Work>
    void VisitShards(size_t start_hint, const std::vector<uint32_t>& shard_of, Work work) const;
    template <typename Work>
    void VisitShardsShared(const std::vector<uint32_t>& shard_of, ThreadPool& pool, size_t helpers, Work work) const;

    struct Wipe;
    struct WipeWorker;
    void RunWipeWorker(Wipe& wipe, WipeWorker& worker) NO_THREAD_SAFETY_ANALYSIS;
    uint64_t WipeShard(Shard& shard, WipeWorker& worker) EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);
    void WriteWipeBatch(const Wipe& wipe, WipeWorker& worker) NO_THREAD_SAFETY_ANALYSIS;

    size_t ShardIndex(const COutPoint& outpoint) const { return (uint64_t{ReadBE32(outpoint.hash.ToUint256().data())} * m_shards.size()) >> 32; }

    template <typename AnyCoin>
    int TagFor(const AnyCoin& coin) const
    {
        const int settled{m_run_in_flight ? m_inflight_settled_height : m_written_height};
        return static_cast<int>(coin.nHeight) > settled ? static_cast<int>(coin.nHeight) : m_change_height;
    }

    mutable std::vector<Shard> m_shards;
    mutable std::array<Totals, TOTALS_STRIPES> m_totals;
    mutable uint256 m_tier_block_hash;
    int m_change_height{0};
    int m_tier_height{-1};
    uint256 m_written_head;
    int m_written_height{-1};
    bool m_run_in_flight{false};
    bool m_flush_failed{false};
    int m_inflight_settled_height{-1};
    bool m_fell_below_written{false};
    std::vector<CoinWrite> m_restored;
    std::atomic<int> m_confirmed_height{-1};
    std::atomic<uint64_t> m_write_out_epoch{0};
    uint64_t m_batch_bytes{DEFAULT_DB_CACHE_BATCH};
    bool m_base_concurrent{false};
    std::shared_ptr<ThreadPool> m_flush_helpers;
};

/**
 * Per-block validation view over the confirmed CoinsStore (plus an optional
 * WindowOverlay for windowed IBD blocks). Validation is strictly read-only on
 * CoinsStore; net dirty changes are extracted pre-bucketed by shard via
 * ExtractChanges() and applied in exact height order by CommitWindowBatch()/CommitBatch().
 */
class BlockApplyView final : public CoinsViewOverlay
{
public:
    BlockApplyView(CoinsStore& store, util::NotNullSharedPtr<ThreadPool> thread_pool, bool deterministic = false);
    ~BlockApplyView() override;

    void PrepareBlock(int height, const WindowOverlay* overlay = nullptr);
    bool InUse() const { return m_in_use; }
    void Prefetch(const CBlock& block);
    bool SpendCoin(const COutPoint& outpoint, Coin* moveto = nullptr) override;
    CoinsStore::BlockChanges ExtractChanges(uint64_t validate_epoch = 0, bool must_be_new = false);
    void Confirm(const uint256& block_hash);
    void Flush(bool reallocate_cache = true) override;
    void Reset() noexcept override;

protected:
    std::optional<Coin> ReadBase(const COutPoint& outpoint) const override;

private:
    CoinsStore& m_store;
    const WindowOverlay* m_overlay{nullptr};
    int m_height{-1};
    bool m_in_use{false};
};

#endif // BITCOIN_COINSSTORE_H
