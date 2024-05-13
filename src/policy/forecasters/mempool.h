// Copyright (c) 2024 The Bitcoin Core developers
// Distributed under the MIT software license. See the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_POLICY_FORECASTERS_MEMPOOL_H
#define BITCOIN_POLICY_FORECASTERS_MEMPOOL_H

#include <policy/fee_estimator.h>
#include <policy/feerate.h>
#include <policy/fees_util.h>

#include <string>
#include <tuple>
#include <vector>

class Chainstate;
class CTxMemPool;

// Fee rate estimates above this confirmation target are not reliable,
// mempool condition might likely change.
const unsigned int MEMPOOL_FORECAST_MAX_TARGET{2};
const std::string MEMPOOL_FORECAST_NAME_STR{"Mempool Forecast"};

/** \class MemPoolForecaster
 * This fee estimate forecaster estimates the fee rate that a transaction will pay
 * to be included in a block as soon as possible.
 * It uses the unconfirmed transactions in the mempool to generate the next block template
 * that will likely be mined.
 * The percentile fee rate's are computed, and the bottom 25th percentile and 50th percentile fee rate's are returned.
 *
 * TODO: Return confidence level of an estimate
 *       Confidence level is determined by some heuristics
 *         - Whether we suspect certain high mining score should confirm but aren't confirming.
 *         - Whether most of the transactions in previously mined blocks were in our mempool.
 *         - Whether our high mining score mempool transactions are confirming.
 */
class MemPoolForecaster : public Forecaster
{
private:
    const CTxMemPool* m_mempool;
    Chainstate* m_chainstate;

public:
    MemPoolForecaster(const CTxMemPool* mempool, Chainstate* chainstate) : m_mempool(mempool), m_chainstate(chainstate){};
    ~MemPoolForecaster() = default;

    /**
     * Estimate the fee rate from mempool transactions given a confirmation target.
     * @param[in] targetBlocks The confirmation target to provide estimate for.
     * @return The forecasted fee rates.
     */
    ForecastResult EstimateFee(unsigned int targetBlocks) override;

    /* Return the maximum confirmation target this forecaster can forecast */
    unsigned int MaxTarget() override
    {
        return MEMPOOL_FORECAST_MAX_TARGET;
    }

private:
    /**
     * @param[in] targetBlocks The confirmation target to provide estimate for.
     * @return The forecasted fee rates.
     */
    ForecastResult EstimateFeeWithMemPool(unsigned int targetBlocks);
};
#endif // BITCOIN_POLICY_FORECASTERS_MEMPOOL_H
