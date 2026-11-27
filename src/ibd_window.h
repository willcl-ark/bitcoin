// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_IBD_WINDOW_H
#define BITCOIN_IBD_WINDOW_H

#include <attributes.h>
#include <coins.h>
#include <coins_sharded.h>
#include <consensus/validation.h>
#include <crypto/siphash.h>
#include <flatfile.h>
#include <node/blockstorage.h>
#include <primitives/block.h>
#include <sync.h>
#include <uint256.h>
#include <util/byte_units.h>
#include <util/hasher.h>
#include <util/time.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <list>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

class CBlockIndex;
class ChainstateManager;
class ThreadPool;
struct BlockValidationContext;

/** Estimate in-memory footprint of a decoded CBlock (transactions, inputs, outputs, scripts, witnesses). */
size_t EstimateBlockMemoryUsage(const CBlock& block) noexcept;

/**
 * Unified auxiliary IBD memory tracker deducted from effective -dbcache headroom
 * in Chainstate::GetCoinsCacheSizeState().
 */
struct IBDMemoryBudget {
    struct Caps {
        size_t max_handoff_bytes{0};
        size_t max_window_blocks_bytes{0};
        size_t max_window_outputs_bytes{0};
        size_t max_pending_undo_bytes{0};
    };

    std::atomic<size_t> handoff_bytes{0};
    std::atomic<size_t> window_blocks_bytes{0};
    std::atomic<size_t> window_outputs_bytes{0};
    std::atomic<size_t> pending_undo_bytes{0};
    std::atomic<size_t> pinned_signals_bytes{0};

    size_t TotalAuxiliaryBytes() const noexcept
    {
        return handoff_bytes.load(std::memory_order_relaxed) +
               window_blocks_bytes.load(std::memory_order_relaxed) +
               window_outputs_bytes.load(std::memory_order_relaxed) +
               pending_undo_bytes.load(std::memory_order_relaxed) +
               pinned_signals_bytes.load(std::memory_order_relaxed);
    }

    static Caps ComputeCaps(size_t coinstip_budget_bytes) noexcept
    {
        // Scale every auxiliary cap proportionally with -dbcache so small caches
        // never over-commit, while bounding peak overhead on large caches.
        return Caps{
            .max_handoff_bytes = std::min<size_t>(coinstip_budget_bytes / 20, 64_MiB),
            .max_window_blocks_bytes = std::min<size_t>(coinstip_budget_bytes / 10, 1024_MiB),
            .max_window_outputs_bytes = std::min<size_t>(coinstip_budget_bytes / 20, 128_MiB),
            .max_pending_undo_bytes = std::min<size_t>(coinstip_budget_bytes / 25, 96_MiB),
        };
    }
};

/**
 * Bounded FIFO in-memory CBlock handoff cache populated by P2P during IBD so
 * ParallelIBDWindow or ConnectTip can avoid re-reading and re-parsing blk*.dat.
 * At chain tip, remains empty and Take() short-circuits with one lock-free atomic check.
 */
class BlockHandoffCache
{
    struct Entry {
        uint256 hash;
        std::shared_ptr<const CBlock> block;
        size_t bytes{0};
    };

    IBDMemoryBudget& m_budget;
    mutable Mutex m_mutex;
    std::list<Entry> m_fifo GUARDED_BY(m_mutex);
    std::unordered_map<uint256, std::list<Entry>::iterator, SaltedUint256Hasher> m_map GUARDED_BY(m_mutex);
    size_t m_total_bytes GUARDED_BY(m_mutex){0};
    std::atomic<size_t> m_count{0};

public:
    explicit BlockHandoffCache(IBDMemoryBudget& budget) : m_budget{budget} {}

    void Put(const uint256& hash, std::shared_ptr<const CBlock> block, size_t max_bytes);
    std::shared_ptr<const CBlock> Take(const uint256& hash);
    void Clear();
    size_t Size() const noexcept { return m_count.load(std::memory_order_relaxed); }
};

/**
 * 64-shard, zero-copy open-addressed per-transaction output table allowing higher
 * blocks in the parallel IBD window to resolve outputs created by earlier uncommitted
 * blocks without copying Coin/CScript objects or blocking the coordinator on cleanup.
 *
 * Published blocks are released in O(1) by the coordinator and swept off-path by
 * worker threads, dropping both hash-table slots and CBlock ownership off b-initload.
 */
class WindowOutputTable
{
public:
    static constexpr size_t NUM_SHARDS = 64;
    static constexpr size_t MIN_RING_SIZE = 256; // power of 2, scaled dynamically for wide windows

    explicit WindowOutputTable(IBDMemoryBudget& budget, size_t min_ring_size = MIN_RING_SIZE);
    ~WindowOutputTable() { Clear(); }

    WindowOutputTable(const WindowOutputTable&) = delete;
    WindowOutputTable& operator=(const WindowOutputTable&) = delete;

    //! Keyed 64-bit non-zero tag for a txid (bit 0 always 1), shared with intra-block tables.
    ALWAYS_INLINE uint64_t HashTxidTag(const Txid& txid) const noexcept { return TagOf(txid); }

    //! Index all transactions of `block` at `height` (zero Coin copies; keeps CBlock alive in ring).
    void InsertBlockOutputs(int height, std::shared_ptr<const CBlock> block);

    //! Batch-lookup outputs created strictly below `consumer_height` and above `m_published`.
    //! If `precomputed_tags` is non-empty, it must match `outpoints.size()` with HashTxidTag(outpoints[i].hash).
    void LookupBatch(int consumer_height,
                     std::span<const COutPoint> outpoints,
                     std::span<std::optional<Coin>> results,
                     std::span<const uint64_t> precomputed_tags = {}) const;

    //! Batched BIP30 check against uncommitted window blocks strictly below `consumer_height`.
    //! Returns the first conflicting spendable outpoint if any txid collides, else nullopt.
    std::optional<COutPoint> FindBIP30ConflictBelowHeight(int consumer_height, const CBlock& block) const;

    //! O(batch_size) coordinator release: advances m_published and queues heights for worker Sweep().
    void ReleaseHeights(int committed_height, std::span<const int> heights);

    //! Sweep any released blocks out of shards and drop their CBlock references on worker threads.
    void Sweep();

    //! Clear all shards, ring slots, and reset memory accounting.
    void Clear();

private:
    struct EntrySlot {
        uint64_t tag{0}; // bit 0 always 1 when occupied; 0 = empty
        uint32_t tx_idx{0};
        int height{0};
    };
    static_assert(sizeof(EntrySlot) == 16);

    struct HeldBlock {
        std::atomic<const CBlock*> ptr{nullptr};
        std::atomic<int> height{-1};
        std::shared_ptr<const CBlock> owner;
    };

    struct alignas(64) Shard {
        mutable std::shared_mutex mutex;
        std::vector<EntrySlot> slots;
        size_t mask{0};
        size_t count{0};
        size_t reported_bytes{0};
    };

    ALWAYS_INLINE uint64_t TagOf(const Txid& txid) const noexcept
    {
        return m_txid_hasher.Hash(txid.ToUint256()) | uint64_t{1};
    }
    static inline size_t ShardOf(uint64_t tag) noexcept { return static_cast<size_t>(tag >> 58); }

    bool EraseOne();
    void UpdateShardBudget(Shard& shard) noexcept;

    IBDMemoryBudget& m_budget;
    SipHasher13UJ m_txid_hasher;
    mutable std::array<Shard, NUM_SHARDS> m_shards;
    const size_t m_ring_size;
    const size_t m_ring_mask;
    std::unique_ptr<HeldBlock[]> m_ring;
    std::atomic<int> m_published{-1};
    mutable Mutex m_released_mutex;
    std::vector<int> m_released GUARDED_BY(m_released_mutex);
    mutable std::atomic<size_t> m_shard_cursor{0};
};

#endif // BITCOIN_IBD_WINDOW_H
