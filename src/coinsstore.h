// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_COINSSTORE_H
#define BITCOIN_COINSSTORE_H

#include <coins.h>
#include <crypto/common.h>
#include <primitives/transaction.h>
#include <sync.h>
#include <uint256.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

class ThreadPool;

/**
 * Sharded in-memory UTXO cache layered above a coins database.
 *
 * Inherits CCoinsViewBacked (not CCoinsViewCache) so CCoinsViewCache remains non-virtual and
 * CoinsStore carries no unused CCoinsMap/PoolResource state or thread-local AccessCoin() trap.
 *
 * Entries are partitioned into key-range shards by the leading bytes of the txid. Each shard
 * stores live UTXOs in fixed 64-entry chunks (eliminating std::vector reallocation spikes) and
 * spent tombstones in separate 36-byte COutPoint chunks, indexed by a single power-of-2 uint32_t
 * open-addressing table with backward-shift deletion. A per-chunk dirty/fresh bitset plus a
 * level-2 dirty-chunk mask makes Flush()/Sync() touch only dirty chunks and keeps small tip
 * Sync() calls O(dirty) without scanning clean entries or rehashing.
 */
class CoinsStore final : public CCoinsViewBacked
{
public:
    static constexpr size_t DEFAULT_SHARDS{256};

    //! Result codes written by FetchInto() for each queried outpoint.
    enum FetchState : uint8_t {
        FETCH_ABSENT = 0, //!< Not present in cache; caller should check the backing DB.
        FETCH_FOUND = 1,  //!< Unspent coin found in cache and written to output.
        FETCH_SPENT = 2,  //!< Spent tombstone in cache; coin is spent, do not query backing DB.
    };

    //! One pre-bucketed UTXO operation for 2-phase CheckShard() / ApplyShardUnconditional().
    struct ShardOp {
        COutPoint outpoint;
        //! Non-null for a create (points into block tx vout); nullptr for a spend.
        const CTxOut* create_out{nullptr};
        //! Coin metadata in serialized layout: `(nHeight << 1) | fCoinBase`.
        uint32_t code{0};
        //! Block index within the CommitJob (0..count-1), non-decreasing within a shard span.
        uint32_t block_idx{0};
        //! Precomputed HashKey(outpoint) so Phase 1 and Phase 2 hash each key once.
        size_t hash{0};
        //! For a create: allow overwriting an existing unspent coin (BIP30 coinbase duplicates).
        bool possible_overwrite{false};
        //! For a spend: input was resolved from base DB and no Flush()/Sync() ran since.
        bool trust_from_base{false};
    };

    explicit CoinsStore(CCoinsView* base_view, size_t shards = DEFAULT_SHARDS, bool deterministic = false);
    ~CoinsStore() override;

    // CCoinsView
    std::optional<Coin> GetCoin(const COutPoint& outpoint) const override;
    std::optional<Coin> PeekCoin(const COutPoint& outpoint) const override;
    bool HaveCoin(const COutPoint& outpoint) const override;
    uint256 GetBestBlock() const override;
    void BatchWrite(CoinsViewCacheCursor& cursor, const uint256& block_hash) override;

    // Tip cache operations
    void SetBestBlock(const uint256& block_hash);
    bool HaveCoinInCache(const COutPoint& outpoint) const;
    void AddCoin(const COutPoint& outpoint, Coin&& coin, bool possible_overwrite);
    void EmplaceCoinInternalDANGER(const COutPoint& outpoint, Coin&& coin);
    bool SpendCoin(const COutPoint& outpoint, Coin* moveto = nullptr);
    void Uncache(const COutPoint& outpoint);
    void Flush(bool reallocate_cache = true, ThreadPool* pool = nullptr);
    void Sync(ThreadPool* pool = nullptr);
    unsigned int GetCacheSize() const;
    size_t GetDirtyCount() const noexcept;
    size_t DynamicMemoryUsage() const;
    void SanityCheck() const;

    size_t ShardCount() const { return m_shards.size(); }
    size_t ShardIndexOf(const COutPoint& outpoint) const { return ShardIndex(outpoint); }
    size_t HashOutpoint(const COutPoint& outpoint) const noexcept { return HashKey(outpoint); }

    //! Monotonic counter incremented at the end of every Flush() and Sync().
    uint64_t FlushGeneration() const noexcept { return m_flush_gen.load(std::memory_order_acquire); }

    //! Read directly from the backing view regardless of what this store holds.
    std::optional<Coin> BaseCoin(const COutPoint& outpoint) const { return base->GetCoin(outpoint); }

    //! Batch lookup for `outpoints` at indices `which`, writing FetchState into `state[w]`
    //! and copying found unspent coins into `coins[w]`.
    void FetchInto(std::span<const COutPoint> outpoints,
                   std::span<const uint32_t> which,
                   std::vector<Coin>& coins,
                   std::vector<uint8_t>& state) const;

    //! Batch BIP30 check: sets `may[i] = 1` if outpoint `i` is unspent in the store or (if absent) in `base`.
    void MayHave(std::span<const COutPoint> outpoints, std::vector<uint8_t>& may) const;

    //! Phase 1 (read-only): verify `ops` for `shard_idx` in order without mutating the store.
    //! Returns the earliest conflicting `block_idx`, or `no_conflict` if all ops are valid.
    uint32_t CheckShard(size_t shard_idx, std::span<const ShardOp> ops, uint32_t no_conflict) const;

    //! Phase 2 (unconditional): apply all ops in `ops` with `op.block_idx < valid_blocks`
    //! under a single acquisition of `m_shards[shard_idx].mutex`.
    void ApplyShardUnconditional(size_t shard_idx, std::span<const ShardOp> ops, uint32_t valid_blocks);

private:
    static constexpr uint32_t EMPTY_SLOT{std::numeric_limits<uint32_t>::max()};
    static constexpr uint32_t TOMB_BIT{uint32_t{1} << 31};
    static constexpr uint32_t INDEX_MASK{~TOMB_BIT};
    static constexpr uint32_t CHUNK_SHIFT{6};
    static constexpr uint32_t CHUNK_SIZE{uint32_t{1} << CHUNK_SHIFT};
    static constexpr uint32_t CHUNK_MASK{CHUNK_SIZE - 1};

    struct PackedEntry {
        COutPoint key;    // 36 bytes
        uint32_t code{0}; // 4 bytes: (nHeight << 1) | fCoinBase
        CTxOut out;       // 48 bytes: CAmount + CScript (prevector<36>)

        uint32_t Height() const noexcept { return code >> 1; }
        bool IsCoinBase() const noexcept { return (code & 1U) != 0; }
        Coin ToCoin() const { return Coin{out, static_cast<int>(Height()), IsCoinBase()}; }
    };
    static_assert(sizeof(PackedEntry) == 88);

    struct LiveChunk {
        uint64_t dirty{0};
        uint64_t fresh{0};
        std::array<PackedEntry, CHUNK_SIZE> entries;
    };

    struct TombChunk {
        std::array<COutPoint, CHUNK_SIZE> keys;
    };

    struct SortKey {
        uint64_t prefix;
        uint32_t tagged; // bit 31 = tombstone, bits 0..30 = position
    };

    struct alignas(64) Shard {
        mutable Mutex mutex;
        std::vector<std::unique_ptr<LiveChunk>> live_chunks GUARDED_BY(mutex);
        std::vector<std::unique_ptr<TombChunk>> tomb_chunks GUARDED_BY(mutex);
        std::vector<uint64_t> dirty_chunk_mask GUARDED_BY(mutex);
        std::vector<uint32_t> index GUARDED_BY(mutex);
        uint32_t live_count GUARDED_BY(mutex){0};
        uint32_t tomb_count GUARDED_BY(mutex){0};
        size_t dirty_live_count GUARDED_BY(mutex){0};
        size_t script_bytes GUARDED_BY(mutex){0};
        //! Incremented when Flush()/Sync() modifies or clears the shard so unlocked base reads retry.
        uint64_t generation GUARDED_BY(mutex){0};

        //! Relaxed snapshots updated once before releasing `mutex` after mutations.
        std::atomic<size_t> a_bytes{0};
        std::atomic<size_t> a_entries{0};
        std::atomic<size_t> a_dirty{0};
    };

    size_t ShardIndex(const COutPoint& outpoint) const
    {
        return (uint64_t{ReadBE32(outpoint.hash.ToUint256().data())} * m_shards.size()) >> 32;
    }
    Shard& ShardFor(const COutPoint& outpoint) const { return m_shards[ShardIndex(outpoint)]; }
    size_t HashKey(const COutPoint& outpoint) const noexcept { return m_hasher(outpoint); }

    static PackedEntry& LiveAt(Shard& shard, uint32_t pos) EXCLUSIVE_LOCKS_REQUIRED(shard.mutex)
    {
        return shard.live_chunks[pos >> CHUNK_SHIFT]->entries[pos & CHUNK_MASK];
    }
    static const PackedEntry& LiveAt(const Shard& shard, uint32_t pos) EXCLUSIVE_LOCKS_REQUIRED(shard.mutex)
    {
        return shard.live_chunks[pos >> CHUNK_SHIFT]->entries[pos & CHUNK_MASK];
    }
    static COutPoint& TombAt(Shard& shard, uint32_t pos) EXCLUSIVE_LOCKS_REQUIRED(shard.mutex)
    {
        return shard.tomb_chunks[pos >> CHUNK_SHIFT]->keys[pos & CHUNK_MASK];
    }
    static const COutPoint& TombAt(const Shard& shard, uint32_t pos) EXCLUSIVE_LOCKS_REQUIRED(shard.mutex)
    {
        return shard.tomb_chunks[pos >> CHUNK_SHIFT]->keys[pos & CHUNK_MASK];
    }
    static const COutPoint& SlotKey(const Shard& shard, uint32_t tagged) EXCLUSIVE_LOCKS_REQUIRED(shard.mutex)
    {
        return (tagged & TOMB_BIT) ? TombAt(shard, tagged & INDEX_MASK) : LiveAt(shard, tagged).key;
    }

    static bool IsLiveDirty(const Shard& shard, uint32_t pos) EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);
    static bool IsLiveFresh(const Shard& shard, uint32_t pos) EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);
    static void SetDirtyChunkBit(Shard& shard, size_t chunk_idx) EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);
    static void MarkLiveDirty(Shard& shard, uint32_t pos) EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);
    static void ClearLiveDirty(Shard& shard, uint32_t pos) EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);
    static void SetLiveFresh(Shard& shard, uint32_t pos, bool fresh) EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);

    static uint32_t AppendLive(Shard& shard, const COutPoint& key) EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);
    static uint32_t AppendTomb(Shard& shard, const COutPoint& key) EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);
    void EraseLiveStorage(Shard& shard, uint32_t pos) const EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);
    void EraseTombStorage(Shard& shard, uint32_t tomb_pos) const EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);

    size_t FindIndexSlot(const Shard& shard, const COutPoint& key, size_t hash) const EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);
    uint32_t FindTagged(const Shard& shard, const COutPoint& key, size_t hash) const EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);
    void Rehash(Shard& shard, size_t new_buckets) const EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);
    void InsertIndexSlot(Shard& shard, const COutPoint& key, size_t hash, uint32_t tagged) const EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);
    void RemoveIndexSlot(Shard& shard, size_t hole) const EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);
    void PublishStats(Shard& shard) const EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);
    static void AssignCoinMove(Shard& shard, PackedEntry& entry, uint32_t code, CAmount value, CScript&& script) EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);
    static void AssignCoinCopy(Shard& shard, PackedEntry& entry, uint32_t code, CAmount value, const CScript& script) EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);
    void SpendLiveAtSlot(Shard& shard, size_t index_slot, Coin* moveto) const EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);
    void ReplaceTombWithLive(Shard& shard, size_t index_slot, uint32_t code, CAmount value, CScript&& script) const EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);
    void ReplaceTombWithLiveCopy(Shard& shard, size_t index_slot, uint32_t code, CAmount value, const CScript& script) const EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);

    std::optional<Coin> Lookup(const COutPoint& outpoint) const;
    void CollectAndSortShard(const Shard& shard, std::vector<SortKey>& keys) const EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);
    void EmitSortedShard(const Shard& shard,
                         std::span<const SortKey> keys,
                         std::vector<SortedCoinWrite>& writes,
                         const std::function<void(std::span<const SortedCoinWrite>)>& emit) const EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);
    void WriteDirtyToBase(const uint256& best, ThreadPool* pool) const;

    template <typename Work>
    void VisitShards(const std::vector<uint32_t>& shard_of, Work work) const;

    const SaltedCoinsCacheHasher m_hasher;
    mutable std::vector<Shard> m_shards;
    std::atomic<uint64_t> m_flush_gen{0};
    uint256 m_store_block_hash;
};

#endif // BITCOIN_COINSSTORE_H
