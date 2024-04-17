#ifndef BITCOIN_ESTIMATOR_H
#define BITCOIN_ESTIMATOR_H

#include <vector>
#include <memory>
#include "forcaster.h"
#include "policy/fees.h"

class FeeEstimator {
private:
    std::vector<std::shared_ptr<Forcaster>> forecasters;

public:
    std::unique_ptr<CBlockPolicyEstimator> legacy_estimator;
    FeeEstimator(std::unique_ptr<CBlockPolicyEstimator> estimator)
        : legacy_estimator(std::move(estimator)) {
    }
    void registerForcaster(std::shared_ptr<Forcaster> forcaster);
    double getFeeRate(int targetBlocks);
    unsigned int HighestTargetTracked();
};

#endif // BITCOIN_ESTIMATOR_H
