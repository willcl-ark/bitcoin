#ifndef BITCOIN_DUMMY_FORCASTER_H
#define BITCOIN_DUMMY_FORCASTER_H

#include <policy/forcaster.h>

class SimpleForcaster : public Forcaster {
public:
    std::pair<double, double> estimate(int targetBlocks) override;
};

#endif // BITCOIN_DUMMY_FORCASTER_H
