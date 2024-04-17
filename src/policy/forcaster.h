#ifndef BITCOIN_FORCASTER_H
#define BITCOIN_FORCASTER_H

#include<utility>

class Forcaster {
public:
    virtual std::pair<double, double> estimate(int targetBlocks) = 0;
    virtual ~Forcaster() {}
};

#endif // BITCOIN_FORCASTER_H
