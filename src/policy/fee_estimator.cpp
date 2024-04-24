// Copyright (c) 2024 The Bitcoin Core developers
// Distributed under the MIT software license. See the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <logging.h>
#include <policy/fee_estimator.h>
#include <policy/feerate.h>
#include <util/trace.h>

void FeeEstimator::RegisterForecaster(std::shared_ptr<Forecaster> forecaster)
{
    forecasters.push_back(forecaster);
}

std::pair<ForecastResult, std::vector<std::string>> FeeEstimator::GetFeeEstimateFromForecasters(unsigned int targetBlocks)
{
    // Request estimates from all registered forecasters and select the lowest
    ForecastResult forecast = ForecastResult("");

    std::vector<std::string> err_messages;
    for (auto& forecaster : forecasters) {
        auto currForecast = forecaster->EstimateFee(targetBlocks);
        if (!currForecast.empty()) {
            if (currForecast < forecast || forecast.empty()) {
                forecast = currForecast;
            }
        } else {
            LogPrint(BCLog::ESTIMATEFEE, "FeeEst: %s.\n", currForecast.m_err_message);
            err_messages.push_back(currForecast.m_err_message);
        }
    }

    if (!forecast.empty()) {
        LogPrint(BCLog::ESTIMATEFEE, "FeeEst: %s, low priority fee rate estimate %s %s/kvB, high priority fee rate estimate %s %s/kvB.\n", forecast.m_forecaster,
                forecast.m_l_priority_estimate.GetFeePerK(), CURRENCY_ATOM, forecast.m_h_priority_estimate.GetFeePerK(), CURRENCY_ATOM);
        TRACE4(fee_estimator, estimate_calculated,
               targetBlocks,
               forecast.m_forecaster,
               forecast.m_l_priority_estimate.GetFeePerK(),
               forecast.m_h_priority_estimate.GetFeePerK());
    }
    return std::make_pair(forecast, err_messages);
};

unsigned int FeeEstimator::MaxForecastingTarget()
{
    unsigned int max_target = 0;
    for (auto& forecaster : forecasters) {
        max_target = std::max(forecaster->MaxTarget(), max_target);
    }
    return max_target;
}
