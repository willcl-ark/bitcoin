// Copyright (c) 2024 The Bitcoin Core developers
// Distributed under the MIT software license. See the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <logging.h>
#include <node/miner.h>
#include <policy/fee_estimator.h>
#include <policy/forecasters/mempool.h>
#include <policy/policy.h>
#include <util/trace.h>


using node::GetNextBlockFeeRateAndVsize;

ForecastResult MemPoolForecaster::EstimateFee(unsigned int targetBlocks)
{
    return EstimateFeeWithMemPool(targetBlocks);
}

ForecastResult MemPoolForecaster::EstimateFeeWithMemPool(unsigned int targetBlocks)
{
    if (targetBlocks <= 0) {
        return ForecastResult("Confirmation target must be greater than zero");
    }

    if (targetBlocks > MEMPOOL_FORECAST_MAX_TARGET) {
        return ForecastResult(strprintf("%s: Confirmation target %s is above maximum limit of %s, mempool conditions might change and forecasts above %s block may be unreliable",
                                        MEMPOOL_FORECAST_NAME_STR, targetBlocks, MEMPOOL_FORECAST_MAX_TARGET, MEMPOOL_FORECAST_MAX_TARGET));
    }

    if (!m_mempool->GetLoadTried()) {
        return ForecastResult(strprintf("%s: Mempool not finished loading; can't get accurate fee rate forecast", MEMPOOL_FORECAST_NAME_STR));
    }

    const auto cached_estimate = cache.get();
    if (cached_estimate) {
        return ForecastResult(cached_estimate->p25, cached_estimate->p50, MEMPOOL_FORECAST_NAME_STR);
    }

    std::vector<std::tuple<CFeeRate, uint64_t>> block_fee_stats;
    {
        LOCK2(cs_main, m_mempool->cs);
        block_fee_stats = GetNextBlockFeeRateAndVsize(*m_chainstate, m_mempool);
    }

    if (block_fee_stats.empty()) {
        return ForecastResult(strprintf("%s: No transactions available in the mempool", MEMPOOL_FORECAST_NAME_STR));
    }

    BlockPercentiles fee_rate_estimate_result = CalculateBlockPercentiles(block_fee_stats);
    if (fee_rate_estimate_result.p75 == CFeeRate(0)) {
        return ForecastResult(strprintf("%s: Not enough transactions in the mempool to provide a fee rate forecast", MEMPOOL_FORECAST_NAME_STR));
    }

    LogPrint(BCLog::ESTIMATEFEE, "FeeEst: %s: Next block 75th percentile fee rate %s %s/kvB, 50th percentile fee rate %s %s/kvB, 25th percentile fee rate %s %s/kvB, 5th percentile fee rate %s %s/kvB \n",
            MEMPOOL_FORECAST_NAME_STR, fee_rate_estimate_result.p75.GetFeePerK(), CURRENCY_ATOM, fee_rate_estimate_result.p50.GetFeePerK(), CURRENCY_ATOM,
            fee_rate_estimate_result.p25.GetFeePerK(), CURRENCY_ATOM, fee_rate_estimate_result.p5.GetFeePerK(), CURRENCY_ATOM);
    TRACE6(feerate_forecast, forecast_generated,
           targetBlocks,
           MEMPOOL_FORECAST_NAME_STR,
           fee_rate_estimate_result.p5.GetFeePerK(),
           fee_rate_estimate_result.p25.GetFeePerK(),
           fee_rate_estimate_result.p50.GetFeePerK(),
           fee_rate_estimate_result.p75.GetFeePerK());
    if (fee_rate_estimate_result.empty()) {
        return ForecastResult(strprintf("%s: Insufficient mempool transactions to provide a fee rate forecast", MEMPOOL_FORECAST_NAME_STR));
    }
    cache.update(fee_rate_estimate_result);
    return ForecastResult(fee_rate_estimate_result.p25, fee_rate_estimate_result.p50, MEMPOOL_FORECAST_NAME_STR);
}
