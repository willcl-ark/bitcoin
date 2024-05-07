// Copyright (c) 2024 The Bitcoin Core developers
// Distributed under the MIT software license. See the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_POLICY_FORECASTERS_BLOCK_H
#define BITCOIN_POLICY_FORECASTERS_BLOCK_H

#include <policy/fees_util.h>
#include <policy/forecaster.h>
#include <validationinterface.h>

#include <queue>


struct RemovedMempoolTransactionInfo;
class Forecaster;
class CValidationInterface;
struct ForecastResult;

const unsigned int MAX_NUMBER_OF_BLOCKS{6};
const std::string BLOCK_FORECAST_NAME_STR{"Block Forecast"};
const unsigned int BLOCK_FORECAST_MAX_TARGET{1};

/** \class BlockForecaster
 * BlockForecaster fee rate forecaster estimates the fee rate that a transaction will pay
 * to be included in a block as soon as possible.
 * BlockForecaster uses the mining score of the transactions that were confirmed in
 * the last MAX_NUMBER_OF_BLOCKS blocks that the node mempool sees.
 * BlockForecaster calculates the MAX_NUMBER_OF_BLOCKS percentiles mining score
 * It returns the average 25th and 50th percentile as the fee rate estimate.
 * TODO: Return confidence level of an estimate
 *       Confidence level is determined by some heuristics
 *          - Whether there are empty blocks in the MAX_NUMBER_OF_BLOCKS.
 *          - Whether most of the last MAX_NUMBER_OF_BLOCKS were seen in the node's mempool
 *
 * TODO: Persist this data to disk and use it upon a quick restart where the last mined block is
 *       less than one.
 */
class BlockForecaster : public CValidationInterface, public Forecaster
{
private:
    std::queue<BlockPercentiles> blocks_percentiles;

protected:
    void MempoolTransactionsRemovedForBlock(const std::vector<RemovedMempoolTransactionInfo>& txs_removed_for_block, unsigned int /*unused*/) override;

public:
    BlockForecaster(){};

    ForecastResult EstimateFee(unsigned int targetBlocks) override;
    unsigned int MaxTarget() override
    {
        return BLOCK_FORECAST_MAX_TARGET;
    }
};
#endif // BITCOIN_POLICY_FORECASTERS_BLOCK_H
