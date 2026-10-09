// Copyright (c) 2026 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_COINS_BATCH_H
#define BITCOIN_COINS_BATCH_H

#include <coins.h>
#include <undo.h>

#include <chrono>
#include <cstddef>
#include <memory>
#include <span>

class CBlock;
class ThreadPool;

/** Material and net cache changes for one bounded group of candidate blocks.
 *
 * The owner keeps the confirmed cache stable and holds cs_main for this object's
 * lifetime. Prepare owns disjoint cache partitions, and joins all workers before
 * returning. Ordinary cache reads must not overlap Prepare. Reads at joined
 * barriers see only confirmed coins and may populate the confirmed cache. The
 * caller checks DynamicMemoryUsage before reserving further validation work.
 * All checks using Inputs() must finish before Commit() or Cancel(). Neither operation
 * requests a flush. Bodies must have passed CheckBlock before admission.
 * Genesis and ambiguous outpoint lifetimes use the serial path.
 */
class CoinsViewBatch
{
public:
    struct BlockDescriptor {
        std::shared_ptr<const CBlock> block;
        int height;
        bool enforce_bip30;
    };

    enum class PrepareResult {
        READY,
        FALLBACK,
        INTERRUPTED,
    };

    /** Known preparation allocations, excluding fetched scripts and undo scripts.
     * The caller additionally reserves block bodies, validation work and bounded
     * per-worker coin-read headroom. Prepare charges actual fetched allocations
     * against scratch_limit and falls back when that allowance is exhausted.
     */
    static size_t EstimateScratch(const CCoinsViewCache& cache, std::span<const BlockDescriptor> blocks);

    CoinsViewBatch(CCoinsViewCache& cache, std::span<const BlockDescriptor> blocks, size_t scratch_limit);
    ~CoinsViewBatch();
    CoinsViewBatch(const CoinsViewBatch&) = delete;
    CoinsViewBatch& operator=(const CoinsViewBatch&) = delete;

    /** Runtime exceptions propagate after submitted workers have been joined. */
    PrepareResult Prepare(ThreadPool& pool);
    std::span<const Coin* const> Inputs(size_t block, size_t tx) const;
    const CBlockUndo& Undo(size_t block) const;
    size_t DynamicMemoryUsage() const;

    struct CommitTimings {
        std::chrono::microseconds promotion;
        std::chrono::microseconds cleanup;
    };

    /** Irrevocable promotion followed by cleanup; all submitted jobs join before
     * publication. Inactive or interrupted pools finish remaining work inline.
     * Ordinary reads must not overlap either worker phase.
     */
    CommitTimings Commit(const uint256& last_hash, ThreadPool& pool) noexcept;
    void Cancel() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

#endif // BITCOIN_COINS_BATCH_H
