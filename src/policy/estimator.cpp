#include "estimator.h"

void FeeEstimator::registerForcaster(std::shared_ptr<Forcaster> forcaster) {
        forecasters.push_back(forcaster);
}

double FeeEstimator::getFeeRate(int targetBlocks) {
    double totalFee = 0.0;
    double totalConfidence = 0.0;
    double weightedFee = 0.0;

    // Request estimates from all registered forecasters
    for (auto& forcaster : forecasters) {
        auto [feeRate, confidence] = forcaster->estimate(targetBlocks);
        weightedFee += feeRate * confidence;
        totalConfidence += confidence;
    }

    // Calculate the final fee estimate, considering the confidence levels
    // Can do other statistical methods here as appropriate
    double finalFeeRate = totalConfidence > 0 ? weightedFee / totalConfidence : 0;

    return finalFeeRate;
};

unsigned int HighestTargetTracked() {
    return 2;
}
