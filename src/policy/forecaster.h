// Copyright (c) 2024 The Bitcoin Core developers
// Distributed under the MIT software license. See the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_POLICY_FORECASTER_H
#define BITCOIN_POLICY_FORECASTER_H

#include <policy/feerate.h>

#include <optional>
#include <string>


enum class ForecastType {

};

struct ForecastResult {
    struct ForecastOptions {
        CFeeRate m_l_priority_estimate{CFeeRate(0)};
        CFeeRate m_h_priority_estimate{CFeeRate(0)};
        unsigned int m_block_height{0};
        ForecastType m_forecaster;
    };

    ForecastOptions m_forecast_opt;
    std::optional<std::string> m_err_message;

    ForecastResult(ForecastResult::ForecastOptions& options, const std::optional<std::string> err_message)
        : m_forecast_opt(options), m_err_message(err_message) {}

    bool empty() const
    {
        return m_forecast_opt.m_l_priority_estimate == CFeeRate(0) && m_forecast_opt.m_h_priority_estimate == CFeeRate(0);
    }

    bool operator<(const ForecastResult& forecast) const
    {
        return m_forecast_opt.m_h_priority_estimate < forecast.m_forecast_opt.m_h_priority_estimate;
    }

    ~ForecastResult() = default;
};

/** \class Forecaster
 * Abstract base class for fee rate forecasters.
 *
 * Derived classes must provide concrete implementations for all virtual methods.
 */
class Forecaster
{
public:
    ForecastType m_forecastType;
    Forecaster(ForecastType forecastType) : m_forecastType(forecastType) {}
    /**
     * Estimate the fee rate required for transaction confirmation.
     *
     * This pure virtual function must be overridden by derived classes to
     * provide a ForecastResult for the specified number of target blocks.
     *
     * @param[in] targetBlocks The number of blocks within which the transaction should be confirmed.
     * @return ForecastResult containing the estimated fee rate.
     */
    virtual ForecastResult EstimateFee(unsigned int targetBlocks) = 0;

    /**
     * Retrieve the maximum target block this forecaster can handle for fee estimation.
     *
     * This pure virtual function must be overridden by derived classes to
     * provide the maximum number of blocks for which a fee rate estimate may
     * be returned.
     *
     * @return unsigned int representing the maximum target block range.
     */
    virtual unsigned int MaxTarget() = 0;

    virtual ~Forecaster() = default;
};

#endif // BITCOIN_POLICY_FORECASTER_H
