// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_COINS_SHARDED_H
#define BITCOIN_COINS_SHARDED_H

#include <coins.h>
#include <memusage.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <sync.h>
#include <uint256.h>
#include <util/check.h>

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <shared_mutex>
#include <span>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <sys/mman.h>
#endif

class ThreadPool;

/**
 * Compare two outpoints in exact LevelDB key serialization order:
 * bytewise txid hash followed by Bitcoin Core VARINT(n) byte encoding.
 */
inline bool CompareOutPointsLevelDBOrder(const COutPoint& a, const COutPoint& b) noexcept
{
    const int cmp = std::memcmp(a.hash.data(), b.hash.data(), 32);
    if (cmp != 0) return cmp < 0;
    if (a.n == b.n) return false;
    if (a.n < 128 && b.n < 128) return a.n < b.n;

    auto encode_varint = [](uint32_t n, uint8_t buf[6]) noexcept -> size_t {
        uint8_t tmp[6];
        size_t len = 0;
        while (true) {
            tmp[len] = static_cast<uint8_t>((n & 0x7Fu) | (len ? 0x80u : 0x00u));
            if (n <= 0x7Fu) break;
            n = (n >> 7) - 1;
            ++len;
        }
        size_t out = 0;
        for (size_t i = len + 1; i-- > 0;) buf[out++] = tmp[i];
        return out;
    };
    uint8_t ba[6];
    uint8_t bb[6];
    const size_t la = encode_varint(a.n, ba);
    const size_t lb = encode_varint(b.n, bb);
    const int vcmp = std::memcmp(ba, bb, la < lb ? la : lb);
    return vcmp != 0 ? vcmp < 0 : la < lb;
}

/**
 * Concurrent, key-range-sharded UTXO cache backing Chainstate::CoinsTip().
 *
 * Partitioned into 256 disjoint shards keyed by outpoint.hash.data()[0] so walking
 * shards 0..255 produces globally sorted LevelDB keys with no cross-shard merge.
 * Inherits CCoinsViewBacked directly rather than CCoinsViewCache, keeping
 * CCoinsViewCache concrete and non-virtual for tip overlays and tests.
 *
 * Concurrency contract: all mutating calls (AddCoin, SpendCoin, Uncache, BatchWrite,
 * CommitBlockBatch, Flush, Sync) and caching GetCoin() must be serialized by a single
 * writer (cs_main in production). Concurrent read-only callers may invoke LookupBatch(),
 * PeekCacheStatusBatch(), PeekCacheStatus(), PeekCoin(), HaveCoinInCache(), and
 * FlushEpoch() without holding cs_main.
 */
class ShardedCoinsView final : public CCoinsViewBacked
{
public:
    static constexpr size_t NUM_SHARDS = 256;
    static constexpr uint32_t MAX_HEIGHT{(uint32_t{1} << 31) - 1};

    static inline size_t ShardIndex(const COutPoint& outpoint) noexcept
    {
        return std::to_integer<size_t>(outpoint.hash.data()[0]);
    }

    enum class CacheLookupStatus : uint8_t {
        ABSENT,
        LIVE,
        TOMBSTONE,
    };

    struct BatchLookupResult {
        CacheLookupStatus status{CacheLookupStatus::ABSENT};
        Coin coin;
    };

    struct BatchCommitOp {
        enum class Kind : uint8_t {
            SPEND,
            CREATE,
        };
        COutPoint outpoint;
        Coin coin;
        uint32_t block_idx{0};
        Kind kind{Kind::SPEND};
        bool from_db{false};
        bool possible_overwrite{false};
    };

    struct BatchCommitBlockMeta {
        uint256 block_hash;
        uint64_t validated_flush_epoch{0};
        uint32_t height{0};
        bool enforce_bip30{false};
    };

    struct BlockSpendOp {
        COutPoint outpoint;
        uint32_t expected_height : 31 {0};
        uint32_t expected_coinbase : 1 {0};
        bool from_db{false};
    };

    /** Lightweight 16-byte POD create descriptor referencing the owning CBlock's CTransaction. */
    struct BlockCreateOp {
        const CTransaction* tx{nullptr};
        uint32_t vout_idx{0};
        uint8_t is_coinbase : 1 {0};
        uint8_t possible_overwrite : 1 {0};
    };
    static_assert(sizeof(BlockCreateOp) <= 16);

    /** Pre-sharded per-block commit slice with spends and creates already grouped by ShardIndex. */
    struct ShardedBlockCommit {
        BatchCommitBlockMeta meta;
        std::span<const BlockSpendOp> spends;
        std::span<const BlockCreateOp> creates;
        const std::array<uint32_t, NUM_SHARDS + 1>* spend_offsets{nullptr};
        const std::array<uint32_t, NUM_SHARDS + 1>* create_offsets{nullptr};
    };

    explicit ShardedCoinsView(CCoinsView* in_base, bool deterministic = false);
    ~ShardedCoinsView() override = default;

    ShardedCoinsView(const ShardedCoinsView&) = delete;
    ShardedCoinsView& operator=(const ShardedCoinsView&) = delete;

    // Standard CCoinsView methods
    std::optional<Coin> GetCoin(const COutPoint& outpoint) const override;
    std::optional<Coin> PeekCoin(const COutPoint& outpoint) const override;
    bool HaveCoin(const COutPoint& outpoint) const override;
    uint256 GetBestBlock() const override EXCLUSIVE_LOCKS_REQUIRED(!m_best_block_mutex);
    void SetBestBlock(const uint256& block_hash) EXCLUSIVE_LOCKS_REQUIRED(!m_best_block_mutex);
    void BatchWrite(CoinsViewCacheCursor& cursor, const uint256& block_hash) override EXCLUSIVE_LOCKS_REQUIRED(!m_best_block_mutex);
    void BatchWriteEntries(
        size_t dirty_count,
        size_t total_count,
        const uint256& block_hash,
        const std::function<void(CoinsWriteSink&)>& emit_entries) override
    {
        CCoinsView::BatchWriteEntries(dirty_count, total_count, block_hash, emit_entries);
    }

    //! Non-caching single-key cache lookup distinguishing ABSENT / LIVE / TOMBSTONE.
    CacheLookupStatus PeekCacheStatus(const COutPoint& outpoint, Coin* coin_out = nullptr) const;

    //! Concurrent batch cache lookup (does not populate cache or query base DB).
    //! Traverses shards in staggered order to avoid mutex convoys across workers.
    void LookupBatch(std::span<const COutPoint> outpoints, std::span<BatchLookupResult> results) const;

    //! Concurrent batch status-only lookup (no Coin deserialization/copying; used by BIP30).
    void PeekCacheStatusBatch(std::span<const COutPoint> outpoints, std::span<CacheLookupStatus> statuses) const;

    //! Check if an unspent coin is currently cached (no backing DB call).
    bool HaveCoinInCache(const COutPoint& outpoint) const;

    //! Add a coin to the cache.
    void AddCoin(const COutPoint& outpoint, Coin&& coin, bool possible_overwrite);

    //! Emplace a coin directly when loading an AssumeUTXO snapshot.
    void EmplaceCoinInternalDANGER(const COutPoint& outpoint, Coin&& coin);

    //! Spend a coin, optionally moving the previous Coin out.
    bool SpendCoin(const COutPoint& outpoint, Coin* moveto = nullptr);

    //! Remove an unmodified coin from the cache.
    void Uncache(const COutPoint& outpoint);

    //! Stream all dirty changes in globally sorted LevelDB key order and clear cache.
    void Flush(bool reallocate_cache = true, ThreadPool* pool = nullptr) EXCLUSIVE_LOCKS_REQUIRED(!m_best_block_mutex);

    //! Stream only dirty entries in globally sorted order while keeping clean live coins.
    //! Removes flushed tombstones without full-table scans or rehashes.
    void Sync(ThreadPool* pool = nullptr) EXCLUSIVE_LOCKS_REQUIRED(!m_best_block_mutex);

    //! Apply a contiguous batch of validated blocks in a single probe per entry,
    //! with lightweight per-shard rollback on rare conflicts.
    //! `shard_ops[s]` must be sorted by `(block_idx, kind)` (SPEND before CREATE).
    //! Returns the number of prefix blocks cleanly committed.
    size_t CommitBlockBatch(
        std::span<const BatchCommitBlockMeta> blocks,
        std::array<std::vector<BatchCommitOp>, NUM_SHARDS>& shard_ops,
        ThreadPool* pool = nullptr) EXCLUSIVE_LOCKS_REQUIRED(!m_best_block_mutex);

    //! Zero-copy overload committing pre-sharded block slices directly without coordinator-thread op copying.
    size_t CommitBlockBatch(
        std::span<const ShardedBlockCommit> blocks,
        ThreadPool* pool = nullptr) EXCLUSIVE_LOCKS_REQUIRED(!m_best_block_mutex);

    //! Monotonic epoch incremented whenever entries or tombstones are removed by Flush/Sync/Uncache.
    uint64_t FlushEpoch() const noexcept { return m_flush_epoch.load(std::memory_order_acquire); }

    //! Total number of cached entries (live + tombstones), matching CCoinsViewCache::GetCacheSize().
    size_t GetCacheSize() const noexcept { return m_cached_coins_count.load(std::memory_order_relaxed); }

    //! Number of dirty entries awaiting write to base.
    size_t GetDirtyCount() const noexcept { return m_dirty_count.load(std::memory_order_relaxed); }

    //! Accurate estimate of dynamic memory usage in bytes across slabs, index tables, dirty lists, and scripts.
    size_t DynamicMemoryUsage() const noexcept { return m_dynamic_memory_usage.load(std::memory_order_relaxed); }

    //! Check whether all prevouts of tx exist unspent in this view.
    bool HaveInputs(const CTransaction& tx) const;

    //! Verify internal invariants across all shards (for tests).
    void SanityCheck() const;

    //! Exposed entry sizes for tests verifying compact packing invariants.
    static constexpr size_t LIVE_ENTRY_BYTES = 80;
    static constexpr size_t TOMBSTONE_ENTRY_BYTES = 40;
    static constexpr size_t INDEX_SLOT_BYTES = 8;

private:
    static constexpr uint8_t FLAG_DIRTY = 1u << 0;
    static constexpr uint8_t FLAG_FRESH = 1u << 1;
    static constexpr uint8_t FLAG_IN_DIRTY_LIST = 1u << 2;
    static constexpr uint8_t FLAG_OCCUPIED = 1u << 3;

    static constexpr uint32_t EMPTY_TAG = 0xFFFFFFFFu;
    static constexpr uint32_t TOMBSTONE_BIT = 0x80000000u;
    static constexpr uint32_t INDEX_MASK = 0x7FFFFFFFu;

    /**
     * Compact 44-byte (align 4) coin representation storing:
     * - 7-byte little-endian amount (52 bits, all valid CAmount up to 2^52 - 2 > 2x MAX_MONEY;
     *   all-1s = spent) with the 4 entry cache flags packed in the upper nibble of m_value[6]
     * - 1-byte script kind: 0..32 inline raw script (P2PKH, P2SH, P2WPKH),
     *   32-byte compact encodings for P2WSH (34 B), P2TR (34 B), compressed P2PK (35 B),
     *   or HEAP pointer to [uint32_t size][script bytes]
     * - 4-byte bitfield packing height (31 bits, matching Coin) and coinbase (1 bit)
     *   so LiveEntry packs to exactly 80 bytes.
     */
    class StoredCoin
    {
        static constexpr uint64_t SPENT_VALUE{(uint64_t{1} << 52) - 1};
        static constexpr unsigned char VALUE_HIGH_MASK{0x0Fu};
        static constexpr unsigned char FLAGS_SHIFT{4};
        static constexpr size_t INLINE{32};
        static constexpr uint8_t P2WSH{INLINE + 1};
        static constexpr uint8_t P2TR{INLINE + 2};
        static constexpr uint8_t P2PK_EVEN{INLINE + 3};
        static constexpr uint8_t P2PK_ODD{INLINE + 4};
        static constexpr uint8_t HEAP{0xff};
        static_assert(P2PK_ODD < HEAP && INLINE >= sizeof(unsigned char*));

        unsigned char m_value[7]{0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x0f};
        uint8_t m_kind{0};
        uint32_t m_height : 31 {0};
        uint32_t m_coinbase : 1 {0};
        unsigned char m_bytes[INLINE]{};

        CAmount Value() const noexcept
        {
            uint64_t value{0};
            for (size_t i = 0; i < 6; ++i) {
                value |= uint64_t{m_value[i]} << (8 * i);
            }
            value |= static_cast<uint64_t>(m_value[6] & VALUE_HIGH_MASK) << 48;
            return value == SPENT_VALUE ? CAmount{-1} : static_cast<CAmount>(value);
        }

        void SetValue(CAmount amount) noexcept
        {
            Assert(amount == -1 || (amount >= 0 && static_cast<uint64_t>(amount) < SPENT_VALUE));
            const uint64_t value{amount == -1 ? SPENT_VALUE : static_cast<uint64_t>(amount)};
            for (size_t i = 0; i < 6; ++i) {
                m_value[i] = static_cast<unsigned char>(value >> (8 * i));
            }
            m_value[6] = static_cast<unsigned char>((m_value[6] & static_cast<unsigned char>(~VALUE_HIGH_MASK)) |
                                                    ((value >> 48) & VALUE_HIGH_MASK));
        }

        unsigned char* HeapBlock() const noexcept
        {
            unsigned char* block;
            std::memcpy(&block, m_bytes, sizeof(block));
            return block;
        }

        size_t HeapSize() const noexcept
        {
            uint32_t size;
            std::memcpy(&size, HeapBlock(), sizeof(size));
            return size;
        }

        size_t Expand(unsigned char* out) const noexcept
        {
            switch (m_kind) {
            case P2WSH:
            case P2TR:
                out[0] = m_kind == P2WSH ? OP_0 : OP_1;
                out[1] = 32;
                std::memcpy(out + 2, m_bytes, 32);
                return 34;
            case P2PK_EVEN:
            case P2PK_ODD:
                out[0] = 33;
                out[1] = m_kind == P2PK_EVEN ? 0x02 : 0x03;
                std::memcpy(out + 2, m_bytes, 32);
                out[34] = OP_CHECKSIG;
                return 35;
            default: {
                const size_t len = m_kind <= INLINE ? size_t{m_kind} : INLINE;
                if (len) std::memcpy(out, m_bytes, len);
                return len;
            }
            }
        }

        void SetScript(const unsigned char* bytes, size_t size)
        {
            if (size <= INLINE) {
                m_kind = static_cast<uint8_t>(size);
                if (size) std::memcpy(m_bytes, bytes, size);
                return;
            }
            if (size == 34 && (bytes[0] == OP_0 || bytes[0] == OP_1) && bytes[1] == 32) {
                m_kind = bytes[0] == OP_0 ? P2WSH : P2TR;
                std::memcpy(m_bytes, bytes + 2, 32);
                return;
            }
            if (size == 35 && bytes[0] == 33 && (bytes[1] == 0x02 || bytes[1] == 0x03) && bytes[34] == OP_CHECKSIG) {
                m_kind = bytes[1] == 0x02 ? P2PK_EVEN : P2PK_ODD;
                std::memcpy(m_bytes, bytes + 2, 32);
                return;
            }
            Assert(size <= std::numeric_limits<uint32_t>::max());
            auto* block = new unsigned char[sizeof(uint32_t) + size];
            const uint32_t size32 = static_cast<uint32_t>(size);
            std::memcpy(block, &size32, sizeof(size32));
            std::memcpy(block + sizeof(uint32_t), bytes, size);
            std::memcpy(m_bytes, &block, sizeof(block));
            m_kind = HEAP;
        }

        void Release() noexcept
        {
            if (m_kind == HEAP) delete[] HeapBlock();
            m_kind = 0;
        }

        void CopyFrom(const StoredCoin& other)
        {
            std::memcpy(m_value, other.m_value, sizeof(m_value));
            m_height = other.m_height;
            m_coinbase = other.m_coinbase;
            if (other.m_kind == HEAP) {
                SetScript(other.HeapBlock() + sizeof(uint32_t), other.HeapSize());
            } else {
                m_kind = other.m_kind;
                std::memcpy(m_bytes, other.m_bytes, INLINE);
            }
        }

        void MoveFrom(StoredCoin& other) noexcept
        {
            std::memcpy(m_value, other.m_value, sizeof(m_value));
            m_kind = other.m_kind;
            m_height = other.m_height;
            m_coinbase = other.m_coinbase;
            std::memcpy(m_bytes, other.m_bytes, INLINE);
            other.m_kind = 0;
            other.SetValue(-1);
            other.SetEntryFlags(0);
            other.m_height = 0;
            other.m_coinbase = 0;
        }

    public:
        static constexpr uint32_t MAX_HEIGHT{(uint32_t{1} << 31) - 1};
        static_assert(MAX_HEIGHT == ShardedCoinsView::MAX_HEIGHT);

        StoredCoin() = default;
        StoredCoin(const StoredCoin& other) { CopyFrom(other); }
        StoredCoin(StoredCoin&& other) noexcept { MoveFrom(other); }
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

        void AssignPreservingFlags(const CTxOut& out, uint32_t height, bool is_coinbase)
        {
            Release();
            SetValue(-1);
            m_height = 0;
            m_coinbase = 0;
            if (out.IsNull()) return;
            Assert(height <= MAX_HEIGHT);
            const CScript& script = out.scriptPubKey;
            SetScript(script.data(), script.size());
            SetValue(out.nValue);
            m_height = height;
            m_coinbase = is_coinbase;
        }

        void AssignPreservingFlags(const Coin& coin)
        {
            if (coin.IsSpent()) {
                Release();
                SetValue(-1);
                m_height = 0;
                m_coinbase = 0;
                return;
            }
            AssignPreservingFlags(coin.out, coin.nHeight, coin.fCoinBase);
        }

        void AssignPreservingFlags(StoredCoin&& other) noexcept
        {
            if (this == &other) return;
            const uint8_t keep_flags = EntryFlags();
            Release();
            std::memcpy(m_value, other.m_value, sizeof(m_value));
            SetEntryFlags(keep_flags);
            m_kind = other.m_kind;
            m_height = other.m_height;
            m_coinbase = other.m_coinbase;
            std::memcpy(m_bytes, other.m_bytes, INLINE);
            other.m_kind = 0;
            other.SetValue(-1);
            other.m_height = 0;
            other.m_coinbase = 0;
        }

        void Clear() noexcept
        {
            Release();
            SetValue(-1);
            m_height = 0;
            m_coinbase = 0;
        }

        Coin ToCoin() const
        {
            Coin coin;
            if (IsSpent()) return coin;
            coin.out.nValue = Value();
            if (m_kind == HEAP) {
                const unsigned char* bytes = HeapBlock() + sizeof(uint32_t);
                coin.out.scriptPubKey.assign(bytes, bytes + HeapSize());
            } else {
                unsigned char bytes[INLINE + 3];
                const size_t size = Expand(bytes);
                coin.out.scriptPubKey.assign(bytes, bytes + size);
            }
            coin.nHeight = m_height;
            coin.fCoinBase = m_coinbase;
            return coin;
        }

        CAmount Amount() const noexcept { return Value(); }
        uint32_t Height() const noexcept { return m_height; }
        bool IsCoinBase() const noexcept { return m_coinbase != 0; }
        bool IsSpent() const noexcept { return Value() == -1; }
        size_t DynamicMemoryUsage() const noexcept
        {
            return m_kind == HEAP ? memusage::MallocUsage(sizeof(uint32_t) + HeapSize()) : 0;
        }

        uint8_t EntryFlags() const noexcept { return static_cast<uint8_t>(m_value[6] >> FLAGS_SHIFT); }
        void SetEntryFlags(uint8_t flags) noexcept
        {
            m_value[6] = static_cast<unsigned char>((m_value[6] & VALUE_HIGH_MASK) | ((flags & 0x0Fu) << FLAGS_SHIFT));
        }
    };
    static_assert(sizeof(StoredCoin) == 44 && alignof(StoredCoin) == 4);

    struct LiveEntry {
        COutPoint outpoint;
        StoredCoin coin;

        uint8_t Flags() const noexcept { return coin.EntryFlags(); }
        void SetFlags(uint8_t f) noexcept { coin.SetEntryFlags(f); }
        void OrFlags(uint8_t mask) noexcept { coin.SetEntryFlags(static_cast<uint8_t>(coin.EntryFlags() | mask)); }
        void AndFlags(uint8_t mask) noexcept { coin.SetEntryFlags(static_cast<uint8_t>(coin.EntryFlags() & mask)); }
    };
    static_assert(sizeof(LiveEntry) == LIVE_ENTRY_BYTES && alignof(LiveEntry) == 4);

    struct TombstoneEntry {
        COutPoint outpoint;
        uint8_t flags{0};
    };
    static_assert(sizeof(TombstoneEntry) == 40 && alignof(TombstoneEntry) == 4);

    struct IndexSlot {
        uint32_t tagged_idx{EMPTY_TAG};
        uint32_t hash32{0};
    };
    static_assert(sizeof(IndexSlot) == 8 && alignof(IndexSlot) == 4);

    static inline void PrefetchRead(const void* ptr) noexcept
    {
#if defined(__GNUC__) || defined(__clang__)
        __builtin_prefetch(ptr, 0, 1);
#else
        (void)ptr;
#endif
    }

    /**
     * Stateless allocator backing allocations >= 2 MiB with 2 MiB-aligned memory advised
     * with MADV_HUGEPAGE on Linux (THP=madvise), eliminating 4 KiB dTLB misses on large
     * UTXO slabs and shard index tables with zero overhead on small allocations.
     */
    template <typename T>
    struct HugePageAllocator {
        using value_type = T;
        static constexpr size_t HUGE_PAGE_BYTES = size_t{2} << 20;

        HugePageAllocator() noexcept = default;
        template <typename U>
        HugePageAllocator(const HugePageAllocator<U>&) noexcept {}

        T* allocate(size_t n)
        {
            if (n > std::numeric_limits<size_t>::max() / sizeof(T)) throw std::bad_array_new_length();
            const size_t bytes = n * sizeof(T);
#if defined(__linux__) && defined(MADV_HUGEPAGE)
            if (bytes >= HUGE_PAGE_BYTES) {
                // Use aligned ::operator new so std::get_new_handler() (e.g. new_handler_terminate)
                // runs consistently on allocation failure while ensuring 2 MiB alignment for THP.
                void* ptr = ::operator new(bytes, std::align_val_t{HUGE_PAGE_BYTES});
                (void)::madvise(ptr, bytes, MADV_HUGEPAGE);
                return static_cast<T*>(ptr);
            }
#endif
            return static_cast<T*>(::operator new(bytes));
        }

        void deallocate(T* ptr, size_t n) noexcept
        {
            if (!ptr) return;
#if defined(__linux__) && defined(MADV_HUGEPAGE)
            if (n * sizeof(T) >= HUGE_PAGE_BYTES) {
                ::operator delete(ptr, std::align_val_t{HUGE_PAGE_BYTES});
                return;
            }
#endif
            ::operator delete(ptr);
        }

        template <typename U>
        bool operator==(const HugePageAllocator<U>&) const noexcept { return true; }
    };

    /**
     * Segmented slab storage growing 64 -> 64 -> 128 -> ... -> 16384 -> fixed 32768 entries.
     * Full LiveEntry slabs (2.5 MiB) receive 2 MiB huge pages on Linux while avoiding
     * 2x reallocation cliffs at multi-GiB scale.
     */
    template <typename T>
    class SegmentedStorage
    {
        static constexpr uint32_t FIRST_SLAB_SIZE = 64;
        static constexpr uint32_t MAX_SLAB_SHIFT = 15; // 32768 entries (2.5 MiB for 80 B LiveEntry)
        static constexpr uint32_t MAX_SLAB_SIZE = 1u << MAX_SLAB_SHIFT;

        using SlabVec = std::vector<T, HugePageAllocator<T>>;
        std::vector<SlabVec> m_slabs;
        std::vector<uint32_t> m_free_list;
        uint32_t m_next_idx{0};
        uint32_t m_capacity{0};
        size_t m_slab_bytes{0};

        static constexpr uint32_t SlabSizeForIndex(size_t slab_idx) noexcept
        {
            if (slab_idx == 0) return FIRST_SLAB_SIZE;
            if (slab_idx <= 9) return FIRST_SLAB_SIZE << (slab_idx - 1);
            return MAX_SLAB_SIZE;
        }

        static constexpr std::pair<size_t, uint32_t> Locate(uint32_t idx) noexcept
        {
            if (idx < MAX_SLAB_SIZE) {
                if (idx < FIRST_SLAB_SIZE) return {0, idx};
                const unsigned bit = static_cast<unsigned>(std::bit_width(idx) - 1);
                return {static_cast<size_t>(bit - 5), idx - (1u << bit)};
            }
            return {static_cast<size_t>(9 + (idx >> MAX_SLAB_SHIFT)), idx & (MAX_SLAB_SIZE - 1)};
        }

    public:
        uint32_t Allocate()
        {
            if (!m_free_list.empty()) {
                const uint32_t idx = m_free_list.back();
                m_free_list.pop_back();
                return idx;
            }
            if (m_next_idx == m_capacity) {
                const uint32_t slab_size = SlabSizeForIndex(m_slabs.size());
                m_slabs.emplace_back(slab_size);
                m_capacity += slab_size;
                m_slab_bytes += static_cast<size_t>(slab_size) * sizeof(T);
            }
            return m_next_idx++;
        }

        void Recycle(uint32_t idx)
        {
            m_free_list.push_back(idx);
        }

        T& operator[](uint32_t idx) noexcept
        {
            const auto [slab, off] = Locate(idx);
            return m_slabs[slab][off];
        }

        const T& operator[](uint32_t idx) const noexcept
        {
            const auto [slab, off] = Locate(idx);
            return m_slabs[slab][off];
        }

        uint32_t HighWaterMark() const noexcept { return m_next_idx; }

        size_t AllocatedBytes() const noexcept
        {
            return m_slab_bytes +
                   m_slabs.capacity() * sizeof(SlabVec) +
                   m_free_list.capacity() * sizeof(uint32_t);
        }

        void Clear(bool release_memory)
        {
            if (release_memory) {
                std::vector<SlabVec>{}.swap(m_slabs);
                std::vector<uint32_t>{}.swap(m_free_list);
                m_next_idx = 0;
                m_capacity = 0;
                m_slab_bytes = 0;
            } else {
                for (uint32_t i = 0; i < m_next_idx; ++i) {
                    (*this)[i] = T{};
                }
                m_next_idx = 0;
                m_free_list.clear();
            }
        }
    };

    struct alignas(64) Shard {
        mutable std::shared_mutex mutex;
        std::vector<IndexSlot, HugePageAllocator<IndexSlot>> table;
        SegmentedStorage<LiveEntry> live_storage;
        SegmentedStorage<TombstoneEntry> tomb_storage;
        std::vector<uint32_t> dirty_live_slots;
        std::vector<uint32_t> dirty_tomb_slots;
        size_t live_count{0};
        size_t tomb_count{0};
        size_t dirty_count{0};
        size_t script_dynamic_bytes{0};
        size_t reported_memory_usage{0};
        size_t reported_coins_count{0};
        size_t reported_dirty_count{0};

        const COutPoint& KeyAt(uint32_t tagged_idx) const noexcept
        {
            if (tagged_idx & TOMBSTONE_BIT) {
                return tomb_storage[tagged_idx & INDEX_MASK].outpoint;
            }
            return live_storage[tagged_idx].outpoint;
        }

        size_t EntryCount() const noexcept { return live_count + tomb_count; }

        size_t ComputeMemoryUsage() const noexcept
        {
            return live_storage.AllocatedBytes() +
                   tomb_storage.AllocatedBytes() +
                   table.capacity() * sizeof(IndexSlot) +
                   dirty_live_slots.capacity() * sizeof(uint32_t) +
                   dirty_tomb_slots.capacity() * sizeof(uint32_t) +
                   script_dynamic_bytes;
        }

        static constexpr size_t NOT_FOUND = static_cast<size_t>(-1);

        size_t FindPos(const COutPoint& outpoint, uint32_t hash32) const noexcept;
        void PrefetchTableSlot(uint32_t hash32) const noexcept
        {
            if (!table.empty()) {
                PrefetchRead(&table[hash32 & (table.size() - 1)]);
            }
        }
        void PrefetchEntry(uint32_t tagged_idx) const noexcept
        {
            if (tagged_idx == EMPTY_TAG) return;
            if (tagged_idx & TOMBSTONE_BIT) {
                PrefetchRead(&tomb_storage[tagged_idx & INDEX_MASK]);
            } else {
                const auto* p = reinterpret_cast<const unsigned char*>(&live_storage[tagged_idx]);
                PrefetchRead(p);
                // 80-byte LiveEntry always spans a second 64-byte cache line.
                PrefetchRead(p + 64);
            }
        }
        void EnsureTableCapacity();
        void InsertIndexSlot(uint32_t tagged_idx, uint32_t hash32);
        void EraseIndexPos(size_t pos) noexcept;

        uint32_t AllocLiveSlot(const COutPoint& outpoint, const CTxOut& out, uint32_t height, bool is_coinbase, uint8_t new_flags);
        uint32_t AllocLiveSlot(const COutPoint& outpoint, const Coin& coin, uint8_t new_flags);
        uint32_t AllocLiveSlotStored(const COutPoint& outpoint, StoredCoin&& coin, uint8_t new_flags);
        void FreeLiveSlot(uint32_t idx, Coin* moveto = nullptr);
        void FreeLiveSlotMoveStored(uint32_t idx, StoredCoin& moveto);
        uint32_t AllocTombSlot(const COutPoint& outpoint, uint8_t new_flags);
        void FreeTombSlot(uint32_t idx);
        void MarkLiveDirty(uint32_t idx);
        void MarkTombDirty(uint32_t idx);
        void Clear(bool release_memory);
    };

    uint32_t HashOutPoint(const COutPoint& outpoint) const noexcept
    {
        const uint64_t h = m_hasher(outpoint);
        return static_cast<uint32_t>(h ^ (h >> 32));
    }

    enum class RestoreKind : uint8_t {
        ERASE_LIVE,
        ERASE_TOMB,
        RESTORE_LIVE,
        RESTORE_LIVE_FROM_TOMB,
        RESTORE_TOMB_FROM_LIVE,
    };

    struct ShardUndoEntry {
        uint32_t block_idx;
        RestoreKind kind;
        uint8_t prev_flags;
        COutPoint outpoint;
        StoredCoin coin;
    };

    struct alignas(64) AlignedUndoVec {
        std::vector<ShardUndoEntry> entries;
    };

    bool ApplyBatchSpend(
        Shard& shard,
        uint32_t block_idx,
        const COutPoint& outpoint,
        uint32_t hash32,
        bool from_db,
        bool epoch_matches,
        uint32_t expected_height,
        bool expected_coinbase,
        bool check_meta,
        std::vector<ShardUndoEntry>& undo) const;

    bool ApplyBatchCreate(
        Shard& shard,
        uint32_t block_idx,
        const COutPoint& outpoint,
        uint32_t hash32,
        const CTxOut& out,
        uint32_t height,
        bool is_coinbase,
        bool possible_overwrite,
        std::vector<ShardUndoEntry>& undo) const;

    bool ApplyBatchCreate(
        Shard& shard,
        uint32_t block_idx,
        const COutPoint& outpoint,
        uint32_t hash32,
        const Coin& coin,
        bool possible_overwrite,
        std::vector<ShardUndoEntry>& undo) const;

    bool RollbackShardUndo(
        Shard& shard,
        uint32_t committed,
        std::vector<ShardUndoEntry>& undo) const;

    void SyncShardAccounting(Shard& shard) const noexcept;

    SaltedCoinsCacheHasher m_hasher;
    mutable Mutex m_best_block_mutex;
    mutable uint256 m_block_hash GUARDED_BY(m_best_block_mutex);

    mutable std::array<Shard, NUM_SHARDS> m_shards;
    mutable std::array<AlignedUndoVec, NUM_SHARDS> m_commit_undos;
    mutable std::atomic<size_t> m_cached_coins_count{0};
    mutable std::atomic<size_t> m_dirty_count{0};
    mutable std::atomic<size_t> m_dynamic_memory_usage{0};
    mutable std::atomic<size_t> m_lookup_shard_cursor{0};
    std::atomic<uint64_t> m_flush_epoch{0};
};

#endif // BITCOIN_COINS_SHARDED_H
