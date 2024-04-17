#include "policy/dummy_forcaster.h"

std::pair<double, double> SimpleForcaster::estimate(int targetBlocks) {
        // This is where the forecaster can do its own forcasting logic
        double start_estimate{15};
        double confidence{0.5};
        double estimate = (start_estimate - targetBlocks);

        return {estimate, confidence};
};
