// Copyright (c) 2024 The Bitcoin Core developers
// Distributed under the MIT software license. See the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <logging.h>
#include <node/miner.h>
#include <policy/fee_estimator.h>
#include <policy/forecasters/mempool.h>
#include <util/trace.h>
#include <validation.h>


ForecastResult MemPoolForecaster::EstimateFee(unsigned int targetBlocks)
{
    ForecastResult::ForecastOptions forecast_options;
    forecast_options.m_forecaster = m_forecastType;
    LOCK2(cs_main, m_mempool->cs);
    forecast_options.m_block_height = m_chainstate->m_chainman.ActiveTip()->nHeight;
    if (targetBlocks > MEMPOOL_FORECAST_MAX_TARGET) {
        return ForecastResult(forecast_options, strprintf("Confirmation target %s is above maximum limit of %s, mempool conditions might change and forecasts above %s block may be unreliable",
                                                          targetBlocks, MEMPOOL_FORECAST_MAX_TARGET, MEMPOOL_FORECAST_MAX_TARGET));
    }

    node::BlockAssembler::Options options = {
        .test_block_validity = false,
    };
    const auto cached_estimate = cache.get();
    if (cached_estimate) {
        forecast_options.m_l_priority_estimate = cached_estimate->p25;
        forecast_options.m_h_priority_estimate = cached_estimate->p50;
        return ForecastResult(forecast_options, std::nullopt);
    }

    node::BlockAssembler assembler(*m_chainstate, m_mempool, options);
    const auto pblocktemplate = assembler.CreateNewBlock(CScript{});
    const auto block_fee_stats = pblocktemplate->vFeeratePerSize;
    BlockPercentiles fee_rate_estimate_result = CalculateBlockPercentiles(block_fee_stats);
    if (fee_rate_estimate_result.empty() || fee_rate_estimate_result.p75 == CFeeRate(0)) {
        return ForecastResult(forecast_options, "No enough transactions in the mempool to provide a feerate forecast");
    }

    LogPrint(BCLog::ESTIMATEFEE, "FeeEst: %s: Block height %s, 75th percentile feerate %s %s/kvB, 50th percentile feerate %s %s/kvB, 25th percentile feerate %s %s/kvB, 5th percentile feerate %s %s/kvB \n",
             forecastTypeToString(m_forecastType), forecast_options.m_block_height, fee_rate_estimate_result.p75.GetFeePerK(), CURRENCY_ATOM, fee_rate_estimate_result.p50.GetFeePerK(), CURRENCY_ATOM,
             fee_rate_estimate_result.p25.GetFeePerK(), CURRENCY_ATOM, fee_rate_estimate_result.p5.GetFeePerK(), CURRENCY_ATOM);

    cache.update(fee_rate_estimate_result);
    forecast_options.m_l_priority_estimate = fee_rate_estimate_result.p25;
    forecast_options.m_h_priority_estimate = fee_rate_estimate_result.p50;
    return ForecastResult(forecast_options, std::nullopt);
}
