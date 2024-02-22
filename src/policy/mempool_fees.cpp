// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2023 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <logging.h>

#include <node/miner.h>
#include <policy/mempool_fees.h>
#include <policy/policy.h>

using node::GetCustomBlockFeeRateHistogram;

MemPoolPolicyEstimator::MemPoolPolicyEstimator() {}

CFeeRate MemPoolPolicyEstimator::EstimateFeeWithMemPool(Chainstate& chainstate, const CTxMemPool& mempool, unsigned int confTarget, const bool force, std::string& err_message) const
{
    std::optional<CFeeRate> cached_fee{std::nullopt};
    std::map<uint64_t, CFeeRate> fee_rates;
    CFeeRate block_fee_rate{0};

    if (confTarget > MAX_CONF_TARGET) {
        err_message = strprintf("Confirmation target %s is above maximum limit of %s, mempool conditions might change and estimates above %s are unreliable.\n", confTarget, MAX_CONF_TARGET, MAX_CONF_TARGET);
        return CFeeRate(0);
    }

    if (!mempool.GetLoadTried()) {
        err_message = "Mempool not finished loading, can't get accurate fee rate estimate.";
        return CFeeRate(0);
    }
    if (!force) {
        cached_fee = cache.get(confTarget);
    }

    if (!cached_fee) {
        std::vector<std::tuple<CFeeRate, uint64_t>> mempool_fee_stats;
        // Always get stats for MAX_CONF_TARGET blocks (3) because current algo
        // fast enough to run that far while we're locked and in here
        {
            LOCK2(cs_main, mempool.cs);
            mempool_fee_stats = GetCustomBlockFeeRateHistogram(chainstate, &mempool, DEFAULT_BLOCK_MAX_WEIGHT * MAX_CONF_TARGET);
        }
        if (mempool_fee_stats.empty()) {
            err_message = "No transactions available in the mempool yet.";
            return CFeeRate(0);
        }
        fee_rates = EstimateBlockFeeRatesWithMempool(mempool_fee_stats, MAX_CONF_TARGET);
        cache.update(fee_rates);
        block_fee_rate = fee_rates[confTarget];
    } else {
        block_fee_rate = *cached_fee;
    }

    if (block_fee_rate == CFeeRate(0)) {
        err_message = "Insufficient mempool transactions to perform an estimate.";
    }
    return block_fee_rate;
}

std::map<uint64_t, CFeeRate> MemPoolPolicyEstimator::EstimateBlockFeeRatesWithMempool(
    const std::vector<std::tuple<CFeeRate, uint64_t>>& mempool_fee_stats, unsigned int confTarget) const
{
    std::map<uint64_t, CFeeRate> fee_rates;
    if (mempool_fee_stats.empty()) return fee_rates;

    auto rstart = mempool_fee_stats.rbegin();
    auto rcur = mempool_fee_stats.rbegin();
    auto rend = mempool_fee_stats.rend();

    size_t block_number{1};
    size_t block_weight{0};

    while (block_number <= confTarget && rcur != rend) {
        size_t tx_weight = std::get<1>(*rcur) * WITNESS_SCALE_FACTOR;
        block_weight += tx_weight;
        auto next_rcur = std::next(rcur);
        if (block_weight >= DEFAULT_BLOCK_MAX_WEIGHT || next_rcur == rend) {
            fee_rates[block_number] = CalculateMedianFeeRate(rstart, rcur);
            block_number++;
            block_weight = 0;
            rstart = next_rcur;
        }
        rcur = next_rcur;
    }
    return fee_rates;
}

CFeeRate MemPoolPolicyEstimator::CalculateMedianFeeRate(
    std::vector<std::tuple<CFeeRate, uint64_t>>::const_reverse_iterator rstart,
    std::vector<std::tuple<CFeeRate, uint64_t>>::const_reverse_iterator rend) const
{
    unsigned int total_weight = 0;
    std::vector<CFeeRate> feeRates;
    for (auto rit = rstart; rit != rend; ++rit) {
        total_weight += std::get<1>(*rit) * WITNESS_SCALE_FACTOR;
        feeRates.push_back(std::get<0>(*rit));
    }

    // Not enough info to provide a decent estimate
    if (total_weight < (DEFAULT_BLOCK_MAX_WEIGHT / 2)) {
        return CFeeRate(0);
    }

    // Calculate median from the collected fee rates
    size_t size = feeRates.size();
    if (size % 2 == 0) {
        auto mid_fee1 = feeRates[size / 2 - 1].GetFeePerK();
        auto mid_fee2 = feeRates[size / 2].GetFeePerK();
        return CFeeRate((mid_fee1 + mid_fee2) / 2);
    } else {
        return feeRates[size / 2];
    }
}
