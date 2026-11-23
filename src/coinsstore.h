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
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

/**
 * The young tier of the UTXO set: the in-memory layer above the coins database
 * holding confirmed coins changed since the last write-out and clean copies of
 * old coins brought in by lookups.
 *
 *  - Sharded by leading bytes of txid (contiguous ranges of database keys) with
 *    one mutex per shard.
 *  - Holds confirmed state only: blocks in the connect window validate read-only
 *    against CoinsStore plus a tiny WindowOverlay holding the window's outputs,
 *    and validated prefixes commit in exact height order per shard across pool
 *    workers (CommitBatch()).
 *  - Every entry carries the height of its last change (or an earlier owed
 *    change height) so lagged write-out runs (BeginWriteOut / StreamWriteOut /
 *    CommitWriteOut) persist the oldest changes without emptying the cache.
 *  - Clean entries can be evicted independently (EvictClean()).
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

class ThreadPool;

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

    //! Index all transactions of `block` (marking intra-block spent outputs as invisible).
    void AddBlockOutputs(int height, std::shared_ptr<const CBlock> block);
    //! Remove transactions of committed (or drained) blocks and release their block references.
    void RemoveBlocks(std::span<const int> heights, ThreadPool* pool = nullptr);
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
        TxEntry* Find(const Txid& txid, uint64_t h) noexcept;
        const TxEntry* Find(const Txid& txid, uint64_t h) const noexcept;
        //! Insert entry; returns pointer to existing entry if txid already present, else nullptr.
        TxEntry* TryInsert(const TxEntry& entry);
        bool Erase(const Txid& txid, uint64_t h, int32_t height) noexcept;
    };
    struct alignas(64) Shard {
        mutable Mutex mutex;
        FlatTable txs GUARDED_BY(mutex);
        //! Rare BIP30 duplicate txids within the same window.
        std::vector<TxEntry> dup_txs GUARDED_BY(mutex);
        //! Rare intra-block spends of outputs with vout index >= 64.
        std::vector<std::pair<COutPoint, int32_t>> high_intra_spends GUARDED_BY(mutex);
    };

    static uint64_t TxHash(const Txid& txid) noexcept
    {
        static const SaltedCoinsCacheHasher hasher;
        return hasher(txid);
    }
    static size_t ShardIndex(const Txid& txid) noexcept
    {
        return (uint64_t{ReadBE32(txid.ToUint256().data())} * NUM_SHARDS) >> 32;
    }
    static std::optional<Coin> ExtractCoin(const Shard& shard, const TxEntry& entry, uint32_t n) EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);

    std::array<Shard, NUM_SHARDS> m_shards;
    std::atomic<size_t> m_total_blocks{0};
    Mutex m_blocks_mutex;
    std::unordered_map<int, std::shared_ptr<const CBlock>> m_blocks GUARDED_BY(m_blocks_mutex);
};

class CoinsStore : public CCoinsViewCache
{
    struct Entry;
    using Node = std::pair<const COutPoint, Entry>;
    struct NodeLink {
        uint32_t number{0};
        NodeLink() = default;
        NodeLink(std::nullptr_t) {}
        NodeLink(const Node* node) : number{Map::NodeNumber(node)} {}
        operator Node*() const { return Map::NodeAt(number); }
        Node* operator->() const { return Map::NodeAt(number); }
    };

public:
    //! One confirmed change to apply in CommitBatch(): an unspent coin creates/overwrites,
    //! a spent coin spends (erasing FRESH or inserting/updating a DIRTY|SPENT marker).
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

    //! Result of CheckBatchConflicts(): contiguous conflict-free prefix length and conflicts on the stopping block.
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
    //! Minimum non-cancelled ops before CommitBatch fans out across pool helpers.
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
    static size_t NodeRegionBytes() { return s_node_region_bytes.load(std::memory_order_relaxed); }
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
    //! Confirmed height published after CommitBatch/ApplyCheckedBlocks + WindowOverlay::RemoveBlocks.
    int ConfirmedHeight() const { return m_confirmed_height.load(std::memory_order_acquire); }
    void PublishConfirmedHeight(int height) { m_confirmed_height.store(height, std::memory_order_release); }
    //! Monotonic counter incremented after every write-out/flush/sync that may drop entries from CoinsStore.
    uint64_t WriteOutEpoch() const { return m_write_out_epoch.load(std::memory_order_acquire); }
    size_t NumShards() const { return m_shards.size(); }

    //! Precompute shard and key hash for an outpoint (callable off cs_main on workers).
    uint32_t ShardOf(const COutPoint& outpoint) const { return static_cast<uint32_t>(ShardIndex(outpoint)); }
    static uint64_t KeyHashOf(const COutPoint& outpoint) noexcept { return KeyHash(outpoint); }

    //! Read-only shard-batched lookup of confirmed coins in the tier; misses are omitted.
    std::vector<std::pair<COutPoint, Coin>> FetchConfirmed(std::vector<COutPoint>&& outpoints) const;

    //! Shard-parallel conflict check and intra-batch create+spend cancellation for pre-bucketed blocks.
    //! Returns the longest conflict-free prefix; if accepted < blocks.size(), clears any cancellations
    //! so only accepted blocks are committed cleanly.
    BatchCheckResult CheckBatchConflicts(std::span<BlockChanges*> blocks, ThreadPool* pool = nullptr) const;

    //! Apply a conflict-free prefix of pre-bucketed blocks in exact height order across shards in parallel.
    void ApplyCheckedBlocks(std::span<BlockChanges*> blocks, const uint256& best_hash, int best_height, ThreadPool* pool = nullptr, bool publish_height = true);

    //! Apply an ordered, conflict-free batch of confirmed changes across shards (in parallel when large).
    //! Sets the tier's best block to `best_hash` / `best_height`; if `publish_height` is false, the caller
    //! must call PublishConfirmedHeight(best_height) after removing the committed blocks from WindowOverlay.
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
    enum List : uint8_t { LIST_NONE, LIST_DIRTY, LIST_CLEAN };

    struct Entry {
        StoredCoin coin;
        int32_t height{0};
        bool dirty : 1 {false};
        bool fresh : 1 {false};
        bool collected : 1 {false};
        bool referenced : 1 {false};
        uint8_t list : 2 {LIST_NONE};
        NodeLink prev;
        NodeLink next;
    };

    static constexpr size_t HUGE_PAGE_BYTES{size_t{2} << 20};
    static void* HintedRegion(size_t bytes, size_t align = HUGE_PAGE_BYTES);
    static inline std::atomic<size_t> s_node_region_bytes{0};
    static constexpr size_t NODE_REGION_BYTES{size_t{32} << 20};
    static constexpr size_t NODE_REGION_AHEAD_FROM{size_t{1} << 30};
    static void* NodeRegion();
    struct RegionAhead;
    static RegionAhead& RegionAheadState();
    static void FreeRegion(void* region, size_t align = HUGE_PAGE_BYTES) noexcept { ::operator delete(region, std::align_val_t{align}); }

    template <typename T>
    struct NodeAllocator {
        using value_type = T;
        static constexpr size_t MAX_KEPT{1024};
        static constexpr size_t REGION_BYTES{NODE_REGION_BYTES};
        static constexpr unsigned SLOT_BITS{static_cast<unsigned>(std::bit_width(REGION_BYTES / sizeof(T)))};
        static constexpr size_t MAX_REGIONS{size_t{1} << (32 - SLOT_BITS)};
        static inline std::atomic<std::byte*> s_regions[MAX_REGIONS]{};
        static uint32_t Number(const T* node)
        {
            static_assert(std::has_single_bit(REGION_BYTES) && SLOT_BITS < 32);
            const auto address{reinterpret_cast<uintptr_t>(node)};
            const uintptr_t base{address & ~uintptr_t{REGION_BYTES - 1}};
            uint32_t region;
            std::memcpy(&region, reinterpret_cast<const void*>(base), sizeof(region));
            return (region << SLOT_BITS) | static_cast<uint32_t>((address - base) / sizeof(T));
        }
        static T* At(uint32_t number)
        {
            if (number == 0) return nullptr;
            std::byte* const base{s_regions[number >> SLOT_BITS].load(std::memory_order_acquire)};
            return reinterpret_cast<T*>(base + (number & ((uint32_t{1} << SLOT_BITS) - 1)) * sizeof(T));
        }
        static void*& NextList(void* list) { return static_cast<void**>(list)[1]; }
        struct Depot {
            Mutex mutex;
            void* lists GUARDED_BY(mutex){nullptr};
            void* loose GUARDED_BY(mutex){nullptr};
            size_t loose_count GUARDED_BY(mutex){0};
            std::byte* fresh GUARDED_BY(mutex){nullptr};
            size_t fresh_bytes GUARDED_BY(mutex){0};
            uint32_t regions GUARDED_BY(mutex){0};
            void Give(void* list) EXCLUSIVE_LOCKS_REQUIRED(!mutex)
            {
                LOCK(mutex);
                NextList(list) = lists;
                lists = list;
            }
            void GiveLoose(void* nodes) EXCLUSIVE_LOCKS_REQUIRED(!mutex)
            {
                LOCK(mutex);
                while (nodes) {
                    void* next{*static_cast<void**>(nodes)};
                    *static_cast<void**>(nodes) = loose;
                    loose = nodes;
                    if (++loose_count == MAX_KEPT) {
                        NextList(loose) = lists;
                        lists = std::exchange(loose, nullptr);
                        loose_count = 0;
                    }
                    nodes = next;
                }
            }
            void* Take() EXCLUSIVE_LOCKS_REQUIRED(!mutex)
            {
                static constexpr size_t LIST_BYTES{MAX_KEPT * sizeof(T)};
                std::byte* nodes;
                {
                    LOCK(mutex);
                    if (lists) return std::exchange(lists, NextList(lists));
                    if (fresh_bytes < LIST_BYTES) {
                        if (regions >= MAX_REGIONS) std::abort();
                        auto* region{static_cast<std::byte*>(NodeRegion())};
                        const uint32_t number{regions++};
                        std::memcpy(region, &number, sizeof(number));
                        s_regions[number].store(region, std::memory_order_release);
                        fresh = region + sizeof(T);
                        fresh_bytes = REGION_BYTES - sizeof(T);
                        s_node_region_bytes.fetch_add(REGION_BYTES, std::memory_order_relaxed);
                    }
                    nodes = fresh;
                    fresh += LIST_BYTES;
                    fresh_bytes -= LIST_BYTES;
                }
                for (size_t i{0}; i < MAX_KEPT; ++i) {
                    *reinterpret_cast<void**>(nodes + i * sizeof(T)) = i + 1 < MAX_KEPT ? nodes + (i + 1) * sizeof(T) : nullptr;
                }
                return nodes;
            }
        };
        static Depot& Shared()
        {
            static Depot* const depot{new Depot};
            return *depot;
        }
        static bool& KeptGone()
        {
            thread_local bool gone{false};
            return gone;
        }
        struct Kept {
            void* head{nullptr};
            size_t count{0};
            void* spare{nullptr};
            ~Kept()
            {
                KeptGone() = true;
                if (spare) Shared().Give(std::exchange(spare, nullptr));
                if (head) Shared().GiveLoose(std::exchange(head, nullptr));
                count = 0;
            }
        };
        static Kept& Mine()
        {
            static_assert(sizeof(T) >= 2 * sizeof(void*) && sizeof(T) % alignof(void*) == 0 && alignof(T) <= alignof(void*));
            thread_local Kept kept;
            return kept;
        }
        NodeAllocator() = default;
        template <typename U>
        NodeAllocator(const NodeAllocator<U>&) noexcept {}
        T* allocate(size_t n)
        {
            Assert(n == 1);
            if (KeptGone()) {
                void* node{Shared().Take()};
                Shared().GiveLoose(std::exchange(*static_cast<void**>(node), nullptr));
                return static_cast<T*>(node);
            }
            Kept& kept{Mine()};
            if (!kept.head) {
                kept.head = kept.spare ? std::exchange(kept.spare, nullptr) : Shared().Take();
                kept.count = MAX_KEPT;
            }
            void* node{kept.head};
            kept.head = *static_cast<void**>(node);
            --kept.count;
            return static_cast<T*>(node);
        }
        void deallocate(T* p, size_t n) noexcept
        {
            Assert(n == 1);
            if (KeptGone()) {
                *reinterpret_cast<void**>(p) = nullptr;
                Shared().GiveLoose(p);
                return;
            }
            Kept& kept{Mine()};
            if (kept.count == MAX_KEPT) {
                if (kept.spare) Shared().Give(kept.spare);
                kept.spare = std::exchange(kept.head, nullptr);
                kept.count = 0;
            }
            *reinterpret_cast<void**>(p) = kept.head;
            kept.head = p;
            ++kept.count;
        }
        friend bool operator==(const NodeAllocator&, const NodeAllocator&) noexcept { return true; }
    };

    static uint64_t KeyHash(const COutPoint& outpoint) noexcept
    {
        static const SaltedCoinsCacheHasher hasher;
        return hasher(outpoint.hash) + outpoint.n;
    }

    class Map
    {
    public:
        using value_type = Node;
        struct Cell {
            uint32_t chain;
            Node node;
        };
        static constexpr size_t NODE_OFFSET{sizeof(uint32_t)};
        static constexpr size_t LINE_BYTES{64};
        static_assert(NODE_OFFSET + sizeof(COutPoint) <= LINE_BYTES && alignof(Node) <= alignof(uint32_t) && sizeof(Cell) == NODE_OFFSET + sizeof(Node));
        static_assert(sizeof(Cell) <= 2 * LINE_BYTES);
        static Cell* CellAt(uint32_t number) { return NodeAllocator<Cell>::At(number); }
        static uint32_t CellNumber(const Cell* cell) { return NodeAllocator<Cell>::Number(cell); }
        static Node* NodeAt(uint32_t number)
        {
            Cell* const cell{CellAt(number)};
            return cell ? &cell->node : nullptr;
        }
        static uint32_t NodeNumber(const Node* node)
        {
            return node ? CellNumber(reinterpret_cast<const Cell*>(reinterpret_cast<const std::byte*>(node) - NODE_OFFSET)) : 0;
        }
        static constexpr size_t BUCKET_BYTES{sizeof(uint32_t)};
        class iterator
        {
        public:
            iterator() = default;
            iterator(const Map* map, size_t bucket, Cell* cell) : m_map{map}, m_bucket{bucket}, m_cell{cell} {}
            Node& operator*() const { return m_cell->node; }
            Node* operator->() const { return &m_cell->node; }
            bool operator==(const iterator& other) const { return m_cell == other.m_cell; }
            iterator& operator++()
            {
                m_cell = CellAt(m_cell->chain);
                while (!m_cell && ++m_bucket < m_map->m_bucket_count) m_cell = CellAt(m_map->m_buckets[m_bucket]);
                return *this;
            }

        private:
            friend class Map;
            const Map* m_map{nullptr};
            size_t m_bucket{0};
            Cell* m_cell{nullptr};
        };

        Map() = default;
        Map(const Map&) = delete;
        Map& operator=(const Map&) = delete;
        ~Map()
        {
            for (size_t b{0}; b < m_bucket_count; ++b) {
                for (Cell* cell{CellAt(m_buckets[b])}; cell;) {
                    Cell* next{CellAt(cell->chain)};
                    Free(cell);
                    cell = next;
                }
            }
        }

        size_t size() const { return m_size; }
        size_t bucket_count() const { return m_bucket_count; }
        iterator end() const { return {}; }
        iterator begin() const
        {
            for (size_t b{0}; b < m_bucket_count; ++b) {
                if (m_buckets[b]) return {this, b, CellAt(m_buckets[b])};
            }
            return {};
        }
        void Prefetch(uint64_t hash) const
        {
            if (m_bucket_count != 0) Ahead(&m_buckets[hash & (m_bucket_count - 1)]);
        }
        void PrefetchNode(uint64_t hash, bool whole) const
        {
            if (m_bucket_count == 0) return;
            if (const Cell* cell{CellAt(m_buckets[hash & (m_bucket_count - 1)])}) {
                const char* const first{reinterpret_cast<const char*>(cell)};
                Ahead(first);
                if (whole) {
                    Ahead(first + LINE_BYTES);
                    Ahead(first + sizeof(Cell) - 1);
                } else {
                    Ahead(first + NODE_OFFSET + sizeof(COutPoint) - 1);
                }
            }
        }
        void PrefetchAll(std::span<const uint64_t> hashes, std::span<const uint32_t> items, auto&& whole) const
        {
            for (const uint32_t i : items) Prefetch(hashes[i]);
            for (const uint32_t i : items) PrefetchNode(hashes[i], whole(i));
        }
        static void PrefetchLinks(const Node& node)
        {
            if (node.second.list == LIST_NONE) return;
            if (const Node* prev{node.second.prev}) Ahead(&prev->second.next, /*to_write=*/true);
            if (const Node* next{node.second.next}) Ahead(&next->second.prev, /*to_write=*/true);
        }
        static void PrefetchWrite(const void* p) { Ahead(p, /*to_write=*/true); }
        iterator find(const COutPoint& key) const { return find(key, KeyHash(key)); }
        iterator find(const COutPoint& key, uint64_t hash) const
        {
            if (m_bucket_count == 0) return {};
            const size_t bucket{hash & (m_bucket_count - 1)};
            for (Cell* cell{CellAt(m_buckets[bucket])}; cell; cell = CellAt(cell->chain)) {
                if (cell->node.first == key) return {this, bucket, cell};
            }
            return {};
        }
        std::pair<iterator, bool> try_emplace(const COutPoint& key) { return try_emplace(key, KeyHash(key)); }
        std::pair<iterator, bool> try_emplace(const COutPoint& key, uint64_t hash)
        {
            if (const iterator it{find(key, hash)}; it != end()) return {it, false};
            if (m_size == m_bucket_count) Rebucket(m_bucket_count == 0 ? MIN_BUCKETS : m_bucket_count * 2);
            const size_t bucket{hash & (m_bucket_count - 1)};
            Cell* cell{NodeAllocator<Cell>{}.allocate(1)};
            new (&cell->node) Node(std::piecewise_construct, std::forward_as_tuple(key), std::tuple<>());
            cell->chain = m_buckets[bucket];
            m_buckets[bucket] = CellNumber(cell);
            ++m_size;
            return {iterator{this, bucket, cell}, true};
        }
        iterator erase(iterator it)
        {
            iterator next{it};
            ++next;
            const uint32_t number{CellNumber(it.m_cell)};
            uint32_t* link{&m_buckets[it.m_bucket]};
            while (*link != number) link = &CellAt(*link)->chain;
            *link = it.m_cell->chain;
            Free(it.m_cell);
            --m_size;
            return next;
        }
        size_t erase(const COutPoint& key)
        {
            const iterator it{find(key)};
            if (it == end()) return 0;
            erase(it);
            return 1;
        }
        void rehash(size_t)
        {
            size_t count{m_size == 0 ? 0 : MIN_BUCKETS};
            while (count < m_size) count *= 2;
            if (count != m_bucket_count) Rebucket(count);
        }

    private:
        static constexpr size_t MIN_BUCKETS{16};
        static void Ahead(const void* p, bool to_write = false)
        {
#if defined(__GNUC__) || defined(__clang__)
            if (to_write) {
                __builtin_prefetch(p, 1);
            } else {
                __builtin_prefetch(p);
            }
#else
            (void)p;
            (void)to_write;
#endif
        }
        static void Free(Cell* cell)
        {
            cell->node.~Node();
            NodeAllocator<Cell>{}.deallocate(cell, 1);
        }
        void Rebucket(size_t count)
        {
            Buckets buckets{nullptr, BucketsDeleter{count * BUCKET_BYTES >= HUGE_PAGE_BYTES}};
            if (count != 0) {
                void* array{buckets.get_deleter().hinted ? HintedRegion(count * BUCKET_BYTES) : ::operator new(count * BUCKET_BYTES)};
                buckets.reset(new (array) uint32_t[count]());
            }
            for (size_t b{0}; b < m_bucket_count; ++b) {
                for (Cell* cell{CellAt(m_buckets[b])}; cell;) {
                    Cell* next{CellAt(cell->chain)};
                    const size_t bucket{KeyHash(cell->node.first) & (count - 1)};
                    cell->chain = buckets[bucket];
                    buckets[bucket] = CellNumber(cell);
                    cell = next;
                }
            }
            m_buckets = std::move(buckets);
            m_bucket_count = count;
        }

        struct BucketsDeleter {
            bool hinted;
            void operator()(uint32_t* array) const noexcept
            {
                if (hinted) {
                    FreeRegion(array);
                } else {
                    ::operator delete(array);
                }
            }
        };
        using Buckets = std::unique_ptr<uint32_t[], BucketsDeleter>;
        Buckets m_buckets{nullptr, BucketsDeleter{false}};
        size_t m_bucket_count{0};
        size_t m_size{0};
    };

    struct Bucket {
        int64_t bytes{0};
        Node* head{nullptr};
    };
    using ShardMutex = Mutex;
    struct alignas(64) Shard {
        mutable ShardMutex mutex;
        Map map GUARDED_BY(mutex);
        size_t dirty_count GUARDED_BY(mutex){0};
        size_t entries GUARDED_BY(mutex){0};
        size_t buckets GUARDED_BY(mutex){0};
        std::vector<Bucket> dirty_spans GUARDED_BY(mutex);
        Node* clean_young GUARDED_BY(mutex){nullptr};
        Node* clean_old GUARDED_BY(mutex){nullptr};
        uint64_t settles GUARDED_BY(mutex){0};
    };

    struct alignas(64) Totals {
        std::atomic<int64_t> bytes{0};
        std::atomic<int64_t> entries{0};
        std::atomic<int64_t> dirty{0};
    };
    static constexpr size_t TOTALS_STRIPES{64};
    Totals& MyTotals() const;
    size_t SumTotals(std::atomic<int64_t> Totals::* field) const;
    static int64_t NodeBytes();
    size_t RegionAheadBytes() const;

    static constexpr int DIRTY_SPAN_BITS{10};
    static size_t SpanOf(int height);
    Bucket& DirtyBucket(Shard& shard, int height) const EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);
    void FreeDirtySpans(Shard& shard) const EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);
    void AddUsage(Shard& shard, size_t bytes) const EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);
    void SubUsage(Shard& shard, size_t bytes) const EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);

    Shard& ShardFor(const COutPoint& outpoint) const { return m_shards[ShardIndex(outpoint)]; }
    static int64_t DirtyBytes(const Entry& entry);
    void Detach(Shard& shard, Node& node) const EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);
    void PrefetchDetach(const Shard& shard, const Node& node) const EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);
    void Attach(Shard& shard, Node& node) const EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);
    static void Reference(Entry& entry) { if (!entry.referenced) entry.referenced = true; }

    using State = coins_entry::State;
    using Event = coins_entry::Event;
    static State StateOf(const Entry& entry)
    {
        using namespace coins_entry;
        return static_cast<State>((entry.dirty ? DIRTY : 0) | (entry.fresh ? FRESH : 0) |
                                  (entry.collected ? COLLECTED : 0) | (entry.coin.IsSpent() ? SPENT : 0));
    }
    static void Enter(Entry& entry, State to, std::optional<int> height)
    {
        using namespace coins_entry;
        Assert(to != REFUSED);
        Assert(static_cast<bool>(to & SPENT) == entry.coin.IsSpent());
        Assert(height.has_value());
        Assert(*height == entry.height || entry.list == LIST_NONE);
        entry.height = *height;
        entry.dirty = to & DIRTY;
        entry.fresh = to & FRESH;
        entry.collected = to & COLLECTED;
    }
    static void Transit(Entry& entry, State from, Event event, int height)
    {
        using namespace coins_entry;
        Enter(entry, Next(from, event), NextHeight(HeightRule(from, event), from, entry.height, height));
    }
    static void Transit(Entry& entry, State from, Event event)
    {
        Assert(coins_entry::HeightRule(from, event) == coins_entry::Height::Keep);
        Enter(entry, coins_entry::Next(from, event), entry.height);
    }
    static void Transit(Entry& entry, Event event, int height) { Transit(entry, StateOf(entry), event, height); }
    static void Transit(Entry& entry, Event event) { Transit(entry, StateOf(entry), event); }

    void CollectChunk(Shard& shard, WriteOutRun& run, uint64_t limit, uint64_t& size, std::vector<CoinWrite>& writes, std::vector<Node*>& nodes) const EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);
    void SettleNode(Shard& shard, Node& node) const EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);
    bool SpendEntry(Shard& shard, Map::iterator it, Coin* moveto) EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);
    std::optional<Coin> Lookup(const COutPoint& outpoint) const;

    template <typename Work>
    void VisitShards(size_t start_hint, const std::vector<uint32_t>& shard_of, Work work) const;
    template <typename Work>
    void VisitShardsShared(const std::vector<uint32_t>& shard_of, ThreadPool& pool, size_t helpers, Work work) const;

    void EraseEntry(Shard& shard, Map::iterator it) const EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);
    void EraseEntry(Shard& shard, Node& node) const EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);
    void EraseDetached(Shard& shard, Map::iterator it) const EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);
    struct Wipe;
    struct WipeWorker;
    void RunWipeWorker(Wipe& wipe, WipeWorker& worker) NO_THREAD_SAFETY_ANALYSIS;
    uint64_t WipeShard(Shard& shard, WipeWorker& worker) EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);
    void WriteWipeBatch(const Wipe& wipe, WipeWorker& worker) NO_THREAD_SAFETY_ANALYSIS;
    void Resized(Shard& shard) const EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);

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
 * CoinsStore; net dirty changes are extracted via ExtractChanges() and applied
 * in exact height order by CoinsStore::CommitBatch().
 */
class BlockApplyView final : public CoinsViewOverlay
{
public:
    BlockApplyView(CoinsStore& store, util::NotNullSharedPtr<ThreadPool> thread_pool, bool deterministic = false);
    ~BlockApplyView() override;

    //! Configure for validating a block at `height` (optionally reading lower window outputs from `overlay`).
    void PrepareBlock(int height, const WindowOverlay* overlay = nullptr);
    //! Whether this view is currently prepared for a block.
    bool InUse() const { return m_in_use; }
    //! Read-only batch prefetch of `block`'s inputs from WindowOverlay and CoinsStore.
    void Prefetch(const CBlock& block);
    //! Preserve spent coin creation height on dirty spent entries for shard-parallel conflict checking.
    bool SpendCoin(const COutPoint& outpoint, Coin* moveto = nullptr) override;
    //! Extract net dirty changes pre-bucketed by shard (on worker thread) and clear the overlay cache.
    CoinsStore::BlockChanges ExtractChanges(uint64_t validate_epoch = 0, bool must_be_new = false);
    //! Commit this single block's changes directly into CoinsStore (used by solo tip path and tests).
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
