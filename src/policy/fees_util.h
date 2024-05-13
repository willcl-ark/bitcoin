// Copyright (c) 2024 The Bitcoin Core developers
// Distributed under the MIT software license. See the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_POLICY_FEES_UTIL_H
#define BITCOIN_POLICY_FEES_UTIL_H

#include <policy/feerate.h>

#include <tuple>
#include <vector>

// Block percentiles fee rate (in sat/kvB).
struct BlockPercentiles {
    CFeeRate p5;  // 5th percentile
    CFeeRate p25; // 25th percentile
    CFeeRate p50; // 50th percentile
    CFeeRate p75; // 75th percentile

    // Default constructor initializes all percentiles to CFeeRate(0).
    BlockPercentiles() : p5(CFeeRate(0)), p25(CFeeRate(0)), p50(CFeeRate(0)), p75(CFeeRate(0)) {}

    // Check if all percentiles are CFeeRate(0).
    bool empty() const
    {
        return p5 == CFeeRate(0) && p25 == CFeeRate(0) && p50 == CFeeRate(0) && p75 == CFeeRate(0);
    }
};

/**
 * Calculate the percentiles feerates.
 *
 * @param[in] fee_rate_stats The fee statistics (fee rate and vsize).
 * @return BlockPercentiles of a given fee statistics.
 */
BlockPercentiles CalculateBlockPercentiles(const std::vector<std::tuple<CFeeRate, uint64_t>>& fee_rate_stats);

#endif // BITCOIN_POLICY_FEES_UTIL_H
