// Copyright (c) 2024 The Bitcoin Core developers
// Distributed under the MIT software license. See the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <logging.h>
#include <policy/fee_estimator.h>
#include <policy/feerate.h>
#include <util/trace.h>

void FeeEstimator::RegisterForecaster(std::shared_ptr<Forecaster> forecaster)
{
    forecasters.emplace(forecaster->m_forecastType, forecaster);
}

std::pair<ForecastResult, std::vector<std::string>> FeeEstimator::GetFeeEstimateFromForecasters(unsigned int targetBlocks)
{
    // Request estimates from all registered forecasters and select the lowest
    ForecastResult::ForecastOptions opts;
    ForecastResult forecast = ForecastResult(opts, std::nullopt);

    std::vector<std::string> err_messages;

    // TODO: Perform sanity checks; call forecasters and select the best.
    if (!forecast.empty()) {
        LogPrint(BCLog::ESTIMATEFEE, "FeeEst: Block height %s, low priority feerate %s %s/kvB, high priority feerate %s %s/kvB.\n",
                 forecast.m_forecast_opt.m_block_height, forecast.m_forecast_opt.m_l_priority_estimate.GetFeePerK(),
                 CURRENCY_ATOM, forecast.m_forecast_opt.m_h_priority_estimate.GetFeePerK(), CURRENCY_ATOM);

    }
    return std::make_pair(forecast, err_messages);
};

unsigned int FeeEstimator::MaxForecastingTarget()
{
    unsigned int max_target = 0;
    for (auto& forecaster : forecasters) {
        max_target = std::max(forecaster.second->MaxTarget(), max_target);
    }
    return max_target;
}
