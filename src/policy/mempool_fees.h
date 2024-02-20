// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2023 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_POLICY_MEMPOOL_FEES_H
#define BITCOIN_POLICY_MEMPOOL_FEES_H

#include <chrono>
#include <map>
#include <optional>
#include <shared_mutex>
#include <string>

#include <logging.h>
#include <policy/feerate.h>

class Chainstate;
class CTxMemPool;

// Fee rate estimates above this confirmation target are not reliable,
// mempool condition might likely change.
static const unsigned int MAX_CONF_TARGET{3};

/**
 * CachedMempoolEstimates holds a cache of recent mempool-based fee estimates.
 * Running the block-building algorithm multiple times is undesriable due to
 * locking.
 */
struct CachedMempoolEstimates {
private:
    // shared_mutex allows for multiple concurrent reads, but only a single update
    mutable std::shared_mutex cache_mutex;
    static constexpr std::chrono::seconds cache_life{30};
    std::map<uint64_t, CFeeRate> estimates;
    std::chrono::steady_clock::time_point last_updated;

    bool isStale() const
    {
        std::shared_lock<std::shared_mutex> lock(cache_mutex);
        return (last_updated + cache_life) < std::chrono::steady_clock::now();
    }

public:
    CachedMempoolEstimates() : last_updated(std::chrono::steady_clock::now() - cache_life - std::chrono::seconds(1)) {}
    CachedMempoolEstimates(const CachedMempoolEstimates&) = delete;
    CachedMempoolEstimates& operator=(const CachedMempoolEstimates&) = delete;

    std::optional<CFeeRate> get(uint64_t number_of_blocks) const
    {
        std::shared_lock<std::shared_mutex> lock(cache_mutex);
        if (isStale()) return std::nullopt;
        LogPrint(BCLog::MEMPOOL, "CachedMempoolEstimates : cache is not stale, using cached value\n");

        auto it = estimates.find(number_of_blocks);
        if (it != estimates.end()) {
            return it->second;
        }
        return std::nullopt;
    }

    void update(const std::map<uint64_t, CFeeRate>& newEstimates)
    {
        std::unique_lock<std::shared_mutex> lock(cache_mutex);
        // Overwrite the entire map with the new data to avoid old
        // estimates remaining.
        estimates = newEstimates;
        last_updated = std::chrono::steady_clock::now();
        LogPrint(BCLog::MEMPOOL, "CachedMempoolEstimates: updated cache\n");
    }
};

/**
 * MemPoolPolicyEstimator estimates the fee rate that a tx should pay
 * to be included in a confirmation target based on the mempool
 * txs and their fee rates.
 *
 * The estimator works by generating template block up to a given confirmation target and then calculate the median
 * fee rate of the txs in the confirmation target block as the approximate fee rate that a tx will pay to
 * likely be included in the block.
 */
class MemPoolPolicyEstimator
{
public:
    MemPoolPolicyEstimator();

    ~MemPoolPolicyEstimator() = default;

    /**
     * Estimate the fee rate from mempool txs data given a confirmation target.
     *
     * @param[in] chainstate The reference to the active chainstate.
     * @param[in] mempool The reference to the mempool from which we will estimate the fee rate.
     * @param[in] confTarget The confirmation target of transactions.
     * @param[out] err_message  optional error message.
     * @return The estimated fee rate.
     */
    CFeeRate EstimateFeeWithMemPool(Chainstate& chainstate, const CTxMemPool& mempool, unsigned int confTarget, const bool force, std::string& err_message) const;

private:
    mutable CachedMempoolEstimates cache;
    /**
     * Calculate the fee rate estimate for blocks of txs up to num_blocks.
     *
     * @param[in] mempool_fee_stats The mempool fee statistics (fee rate and size).
     * @param[in] num_blocks The numbers of blocks to calculate fees for.
     * @return The fee rate estimate in satoshis per kilobyte.
     */
    std::map<uint64_t, CFeeRate> EstimateBlockFeeRatesWithMempool(const std::map<CFeeRate, uint64_t>& mempool_fee_stats, unsigned int num_blocks) const;

    /**
     * Calculate the median fee rate for a range of txs in the mempool.
     *
     * @param[in] start_it The iterator pointing to the beginning of the range.
     * @param[in] end_it The iterator pointing to the end of the range.
     * @return The median fee rate.
     */
    CFeeRate CalculateMedianFeeRate(std::map<CFeeRate, uint64_t>::const_reverse_iterator start_it, std::map<CFeeRate, uint64_t>::const_reverse_iterator end_it) const;
};
#endif // BITCOIN_POLICY_MEMPOOL_FEES_H
