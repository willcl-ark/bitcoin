// Copyright (c) 2024 The Bitcoin Core developers
// Distributed under the MIT software license. See the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_POLICY_FEE_ESTIMATOR_H
#define BITCOIN_POLICY_FEE_ESTIMATOR_H

#include <policy/fees.h>
#include <policy/forecaster.h>
#include <util/fs.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

/** \class FeeEstimator
 * Module for managing and utilising multiple fee rate forecasters to provide fee estimates.
 *
 * The FeeEstimator class allows for the registration of multiple fee rate
 * forecasters. When an estimate is requested, all registered forecasters are
 * polled, and the lowest estimate with acceptable confidence is selected.
 */
class FeeEstimator
{
private:
    //! Vector of shared pointers to registered forecasters.
    std::vector<std::shared_ptr<Forecaster>> forecasters;

public:
    //! Optional unique ptr to legacy block policy estimator.
    std::optional<std::unique_ptr<CBlockPolicyEstimator>> legacy_estimator;

    /**
     * Constructor that initialises with a legacy estimator
     *
     * @param[in] estimation_filepath Path to the estimation file.
     * @param[in] read_stale_estimates Boolean flag indicating whether to read stale estimates.
     */
    FeeEstimator(const fs::path& estimation_filepath, const bool read_stale_estimates)
        : legacy_estimator(std::make_unique<CBlockPolicyEstimator>(estimation_filepath, read_stale_estimates))
    {
    }

    /**
     * Default constructor that initialises without a legacy estimator.
     */
    FeeEstimator() : legacy_estimator(std::nullopt) {}

    ~FeeEstimator() = default;

    /**
     * Register a forecaster to provide fee rate estimates.
     *
     * @param[in] forecaster Shared pointer to a Forecaster instance to be registered.
     */
    void RegisterForecaster(std::shared_ptr<Forecaster> forecaster);

    /**
     * Get a fee rate estimate from all registered forecasters for a given target block count.
     *
     * Polls all registered forecasters and selects the lowest fee rate
     * estimate with acceptable confidence.
     *
     * @param[in] targetBlocks The number of blocks within which the transaction should be confirmed.
     * @return A pair consisting of the forecast result and a vector of forecaster names.
     */
    std::pair<ForecastResult, std::vector<std::string>> GetFeeEstimateFromForecasters(unsigned int targetBlocks);

    /**
     * Retrieve the maximum target block range across all registered forecasters.
     *
     * @return The maximum number of blocks for which any forecaster can provide a fee rate estimate.
     */
    unsigned int MaxForecastingTarget();
};

#endif // BITCOIN_POLICY_FEE_ESTIMATOR_H
