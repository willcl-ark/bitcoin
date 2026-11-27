// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_IBD_WINDOW_H
#define BITCOIN_IBD_WINDOW_H

#include <coins.h>
#include <coins_sharded.h>
#include <consensus/validation.h>
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

#endif // BITCOIN_IBD_WINDOW_H
