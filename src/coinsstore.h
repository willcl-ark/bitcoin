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

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <utility>
#include <vector>

/**
 * Sharded in-memory UTXO cache layered above a coins database.
 *
 * Implements the CCoinsViewCache interface while partitioning entries into key-range shards by the
 * leading bytes of the txid. Each shard stores a dense vector of packed entries indexed by a
 * power-of-2 uint32_t open-addressing table with backward-shift deletion, giving uniform ~94 B/UTXO
 * memory usage across Linux, macOS, and Windows with zero allocation when empty.
 *
 * Mutations are serialized by the owner (under cs_main) or partitioned across disjoint shard groups
 * during parallel commits; per-shard locks make concurrent lookups from validation worker threads
 * safe. AccessCoin() returns a reference to a thread-local copy valid until the calling thread's
 * next AccessCoin() call.
 */
class CoinsStore : public CCoinsViewCache
{
public:
    static constexpr size_t DEFAULT_SHARDS{256};

    explicit CoinsStore(CCoinsView* base_view, size_t shards = DEFAULT_SHARDS, bool deterministic = false);
    ~CoinsStore() override;

    // CCoinsView
    std::optional<Coin> GetCoin(const COutPoint& outpoint) const override;
    std::optional<Coin> PeekCoin(const COutPoint& outpoint) const override;
    bool HaveCoin(const COutPoint& outpoint) const override;
    uint256 GetBestBlock() const override;
    void BatchWrite(CoinsViewCacheCursor& cursor, const uint256& block_hash) override;

    // CCoinsViewCache
    void SetBestBlock(const uint256& block_hash) override;
    bool HaveCoinInCache(const COutPoint& outpoint) const override;
    const Coin& AccessCoin(const COutPoint& outpoint) const override;
    void AddCoin(const COutPoint& outpoint, Coin&& coin, bool possible_overwrite) override;
    void EmplaceCoinInternalDANGER(const COutPoint& outpoint, Coin&& coin) override;
    bool SpendCoin(const COutPoint& outpoint, Coin* moveto = nullptr) override;
    void Flush(bool reallocate_cache = true) override;
    void Sync() override;
    void Uncache(const COutPoint& outpoint) override;
    unsigned int GetCacheSize() const override;
    size_t GetDirtyCount() const noexcept override;
    size_t DynamicMemoryUsage() const override;
    void SanityCheck() const override;
    void Reset() noexcept override;

    //! One confirmed UTXO mutation for ApplyConfirmed()/ApplyShard(): an unspent coin creates an
    //! output and a spent coin spends it. The caller owns the pointed-to outpoint, coin, and optional
    //! script for the duration of the call.
    struct Change {
        const COutPoint* outpoint;
        Coin* coin;
        //! For a create: allow overwriting an existing unspent coin (BIP30 coinbase duplicates).
        bool possible_overwrite{false};
        //! For a spend: `coin` carries the height and coinbase flag the block validated against; a
        //! mismatch in the store is reported as a conflict.
        bool known_coin{false};
        //! For a spend: the input was resolved directly from the backing DB and no Flush()/Sync()
        //! has occurred since, so a cache miss can record a spent tombstone without re-reading base.
        bool trust_from_base{false};
        //! Optional script for a create when `coin->out.scriptPubKey` is left empty to avoid copying.
        const CScript* script{nullptr};
    };

    //! Conflict and overwrite details reported by ApplyConfirmed()/ApplyShard() so a caller can roll back.
    struct ConfirmedReport {
        std::vector<const COutPoint*> conflicted;
        std::vector<std::pair<const COutPoint*, Coin>> overwritten;
        std::vector<const COutPoint*> unread;
    };

    /**
     * Apply a block's validated changes across shards. Conflicting changes (a spend whose coin is
     * missing or differs from the expected height/coinbase flag, or a create over an unspent coin
     * without possible_overwrite) are skipped and counted in the return value (and recorded in
     * `report` when non-null). Non-conflicting changes are applied.
     */
    uint32_t ApplyConfirmed(std::vector<Change>&& changes,
                            ConfirmedReport* report = nullptr,
                            const std::function<const Coin*(const COutPoint&)>& base_coin = {});

    /**
     * Apply changes that all map to `shard_idx`, acquiring `m_shards[shard_idx].mutex` once for the
     * entire span (and once more only if any non-trusted spend misses the cache).
     */
    uint32_t ApplyShard(size_t shard_idx,
                        std::span<const Change> changes,
                        ConfirmedReport* report = nullptr,
                        const std::function<const Coin*(const COutPoint&)>& base_coin = {});

    size_t ShardCount() const { return m_shards.size(); }
    size_t ShardIndexOf(const COutPoint& outpoint) const { return ShardIndex(outpoint); }

    //! Monotonic counter incremented at the end of every Flush() and Sync().
    uint64_t FlushGeneration() const noexcept { return m_flush_gen.load(std::memory_order_acquire); }

    //! Read directly from the backing view regardless of what this store holds.
    std::optional<Coin> BaseCoin(const COutPoint& outpoint) const { return base->GetCoin(outpoint); }

    //! Batch lookup for `outpoints` at indices `which`, writing found coins into `coins[w]` and setting `found[w] = 1`.
    void FetchInto(std::span<const COutPoint> outpoints,
                   std::span<const uint32_t> which,
                   std::vector<Coin>& coins,
                   std::vector<uint8_t>& found) const;

    //! Batch BIP30 check: sets `may[i] = 1` if outpoint `i` has an entry in the store or exists in `base`.
    void MayHave(std::span<const COutPoint> outpoints, std::vector<uint8_t>& may) const;

private:
    static constexpr uint32_t EMPTY_SLOT{std::numeric_limits<uint32_t>::max()};
    static constexpr uint32_t META_COINBASE{1U << 0};
    static constexpr uint32_t META_DIRTY{1U << 1};
    static constexpr uint32_t META_FRESH{1U << 2};
    static constexpr uint32_t META_HEIGHT_SHIFT{3};
    static constexpr uint32_t MAX_PACKED_HEIGHT{(1U << (32 - META_HEIGHT_SHIFT)) - 1};

    struct PackedEntry {
        COutPoint key;
        uint32_t meta{0};
        CAmount value{-1};
        CScript script;

        bool IsSpent() const noexcept { return value == -1; }
        bool IsDirty() const noexcept { return (meta & META_DIRTY) != 0; }
        bool IsFresh() const noexcept { return (meta & META_FRESH) != 0; }
        bool IsCoinBase() const noexcept { return (meta & META_COINBASE) != 0; }
        uint32_t Height() const noexcept { return meta >> META_HEIGHT_SHIFT; }

        void SetFlags(bool dirty, bool fresh) noexcept
        {
            meta = (meta & ~(META_DIRTY | META_FRESH)) | (dirty ? META_DIRTY : 0U) | (fresh ? META_FRESH : 0U);
        }

        Coin ToCoin() const
        {
            if (IsSpent()) return Coin{};
            return Coin{CTxOut{value, script}, static_cast<int>(Height()), IsCoinBase()};
        }

        void ClearCoin() noexcept
        {
            value = -1;
            CScript{}.swap(script);
            meta &= (META_DIRTY | META_FRESH);
        }
    };

    struct alignas(64) Shard {
        mutable Mutex mutex;
        std::vector<PackedEntry> entries GUARDED_BY(mutex);
        std::vector<uint32_t> index GUARDED_BY(mutex);
        size_t dirty_count GUARDED_BY(mutex){0};
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

    uint32_t FindEntry(const Shard& shard, const COutPoint& key, size_t hash) const EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);
    void Rehash(Shard& shard, size_t new_buckets) const EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);
    uint32_t InsertOrFind(Shard& shard, const COutPoint& key, size_t hash, bool& inserted) const EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);
    void EraseEntry(Shard& shard, uint32_t pos) const EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);
    void PublishStats(Shard& shard) const EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);
    void MarkDirty(Shard& shard, PackedEntry& entry) const EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);
    void ClearDirty(Shard& shard, PackedEntry& entry) const EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);
    void AssignCoin(Shard& shard, PackedEntry& entry, CAmount value, uint32_t height, bool coinbase, CScript&& script) const EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);
    void AssignCoinCopy(Shard& shard, PackedEntry& entry, CAmount value, uint32_t height, bool coinbase, const CScript& script) const EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);
    void SpendEntryLocked(Shard& shard, uint32_t pos, Coin* moveto) const EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);

    std::optional<Coin> Lookup(const COutPoint& outpoint) const;
    void WriteDirtyToBase(const uint256& best) const;

    template <typename Work>
    void VisitShards(const std::vector<uint32_t>& shard_of, Work work) const;

    const SaltedCoinsCacheHasher m_hasher;
    mutable std::vector<Shard> m_shards;
    std::atomic<uint64_t> m_flush_gen{0};
    uint256 m_store_block_hash;
};

#endif // BITCOIN_COINSSTORE_H
