// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_COINSSTORE_H
#define BITCOIN_COINSSTORE_H

#include <coins.h>
#include <crypto/common.h>
#include <primitives/transaction.h>
#include <support/allocators/pool.h>
#include <sync.h>
#include <uint256.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

/**
 * Sharded in-memory UTXO cache layered above a coins database.
 *
 * Implements the CCoinsViewCache interface while partitioning entries into key-range shards by the
 * leading bytes of the txid. Each shard owns its own mutex and PoolResource so lookups and confirmed
 * block commits on disjoint shards run concurrently without contention, and Flush()/Sync() emit
 * sorted, disjoint key-range chunks to the backing CCoinsView via BatchWriteSorted().
 *
 * Mutations are serialized by the owner (under cs_main); per-shard locks make concurrent lookups from
 * validation worker threads safe. AccessCoin() returns a reference to a thread-local copy valid until
 * the calling thread's next AccessCoin() call.
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

    //! One confirmed UTXO mutation for ApplyConfirmed(): an unspent coin creates an output and a spent
    //! coin spends it. The caller owns the pointed-to outpoint, coin, and optional script for the call.
    struct Change {
        const COutPoint* outpoint;
        Coin* coin;
        //! For a create: allow overwriting an existing unspent coin (BIP30 coinbase duplicates).
        bool possible_overwrite{false};
        //! For a spend: `coin` carries the height and coinbase flag the block validated against; a
        //! mismatch in the store is reported as a conflict.
        bool known_coin{false};
        //! Optional script for a create when `coin->out.scriptPubKey` is left empty to avoid copying.
        const CScript* script{nullptr};
    };

    //! Conflict and overwrite details reported by ApplyConfirmed() so a caller can roll back a failed batch.
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
     *
     * When a spend misses the in-memory store, `base_coin` (if provided) supplies the coin read from
     * the database (e.g. from undo data); otherwise the store fetches it from `base`.
     */
    uint32_t ApplyConfirmed(std::vector<Change>&& changes,
                            ConfirmedReport* report = nullptr,
                            const std::function<const Coin*(const COutPoint&)>& base_coin = {});

    size_t ShardCount() const { return m_shards.size(); }
    size_t ShardIndexOf(const COutPoint& outpoint) const { return ShardIndex(outpoint); }

    //! Read directly from the backing view regardless of what this store holds.
    std::optional<Coin> BaseCoin(const COutPoint& outpoint) const { return base->GetCoin(outpoint); }

    //! Batch lookup of outpoints currently cached as unspent in this store, taking each shard lock once.
    std::vector<std::pair<COutPoint, Coin>> Fetch(std::vector<COutPoint>&& outpoints) const;

    //! Batch lookup for `outpoints` at indices `which`, writing found coins into `coins[w]` and setting `found[w] = 1`.
    void FetchInto(std::span<const COutPoint> outpoints,
                   std::span<const uint32_t> which,
                   std::vector<Coin>& coins,
                   std::vector<uint8_t>& found) const;

    //! Batch BIP30 check: sets `may[i] = 1` if outpoint `i` has an entry in the store or exists in `base`.
    void MayHave(std::span<const COutPoint> outpoints, std::vector<uint8_t>& may) const;

private:
    struct Entry {
        Coin coin;
        bool dirty{false};
        bool fresh{false};
    };

    using ShardPair = std::pair<const COutPoint, Entry>;
    static constexpr size_t SHARD_POOL_MAX_BLOCK_BYTES{
        (sizeof(ShardPair) + sizeof(void*) * 4 + alignof(void*) - 1) & ~(alignof(void*) - 1)};
    static constexpr size_t SHARD_POOL_CHUNK_BYTES{16384};
    using ShardAllocator = PoolAllocator<ShardPair, SHARD_POOL_MAX_BLOCK_BYTES>;
    using ShardMemoryResource = ShardAllocator::ResourceType;
    using ShardMap = std::unordered_map<COutPoint, Entry, SaltedCoinsCacheHasher, std::equal_to<COutPoint>, ShardAllocator>;

    struct alignas(64) Shard {
        mutable Mutex mutex;
        std::optional<ShardMemoryResource> resource GUARDED_BY(mutex);
        std::optional<ShardMap> map GUARDED_BY(mutex);
        size_t dirty_count GUARDED_BY(mutex){0};
        size_t entries GUARDED_BY(mutex){0};
        size_t map_bytes GUARDED_BY(mutex){0};
        //! Incremented when Flush()/Sync() modifies or clears the shard so unlocked base reads retry.
        uint64_t generation GUARDED_BY(mutex){0};
    };

    struct alignas(64) Totals {
        std::atomic<int64_t> bytes{0};
        std::atomic<int64_t> entries{0};
        std::atomic<int64_t> dirty{0};
    };
    static constexpr size_t TOTALS_STRIPES{64};

    Totals& MyTotals() const;
    size_t SumTotals(std::atomic<int64_t> Totals::* field) const;

    size_t ShardIndex(const COutPoint& outpoint) const
    {
        return (uint64_t{ReadBE32(outpoint.hash.ToUint256().data())} * m_shards.size()) >> 32;
    }
    Shard& ShardFor(const COutPoint& outpoint) const { return m_shards[ShardIndex(outpoint)]; }

    ShardMap& EnsureMap(Shard& shard) const EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);
    void Resized(Shard& shard) const EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);
    void AddUsage(Shard& shard, size_t bytes) const EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);
    void SubUsage(Shard& shard, size_t bytes) const EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);
    void MarkDirty(Shard& shard, Entry& entry) const EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);
    void ClearDirty(Shard& shard, Entry& entry) const EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);

    std::optional<Coin> Lookup(const COutPoint& outpoint) const;
    void SpendEntryLocked(Shard& shard, ShardMap::iterator it, Coin* moveto) EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);
    void WriteDirtyToBase(const uint256& best) const;

    template <typename Work>
    void VisitShards(const std::vector<uint32_t>& shard_of, Work work) const;

    const bool m_deterministic;
    mutable std::vector<Shard> m_shards;
    mutable std::array<Totals, TOTALS_STRIPES> m_totals;
    mutable uint256 m_store_block_hash;
};

#endif // BITCOIN_COINSSTORE_H
