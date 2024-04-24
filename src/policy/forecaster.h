// Copyright (c) 2024 The Bitcoin Core developers
// Distributed under the MIT software license. See the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_POLICY_FORECASTER_H
#define BITCOIN_POLICY_FORECASTER_H

#include <policy/feerate.h>

#include <string>

struct ForecastResult {
    CFeeRate m_l_priority_estimate;
    CFeeRate m_h_priority_estimate;
    std::string m_forecaster;
    std::string m_err_message;

    ForecastResult(const CFeeRate& l_priority_estimate, const CFeeRate& h_priority_estimate, const std::string& forecaster)
        : m_l_priority_estimate(l_priority_estimate), m_h_priority_estimate(h_priority_estimate), m_forecaster(forecaster) {}

    ForecastResult(const std::string& err_message)
        : m_l_priority_estimate(CFeeRate(0)), m_h_priority_estimate(CFeeRate(0)), m_err_message(err_message) {}

    bool empty() const
    {
        return m_l_priority_estimate == CFeeRate(0) && m_h_priority_estimate == CFeeRate(0);
    }

    bool operator<(const ForecastResult& forecast) const
    {
        return m_h_priority_estimate < forecast.m_h_priority_estimate;
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
