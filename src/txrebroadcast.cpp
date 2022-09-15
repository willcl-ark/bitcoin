// Copyright (c) 2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <consensus/consensus.h>
#include <node/miner.h>
#include <node/blockstorage.h>
#include <script/script.h>
#include <validation.h>
#include <txrebroadcast.h>

using node::BlockAssembler;
using node::ReadBlockFromDisk;

/** We rebroadcast up to 3/4 of max block weight to reduce noise due to
 * circumstances such as miners mining priority transactions. */
static constexpr float REBROADCAST_WEIGHT_RATIO{0.75};

std::vector<TxIds> TxRebroadcastHandler::GetRebroadcastTransactions(const std::shared_ptr<const CBlock>& recent_block, const CBlockIndex& recent_block_index)
{

    // Calculate how many transactions to rebroadcast based on the size of the
    // incoming block.
    float rebroadcast_block_weight = REBROADCAST_WEIGHT_RATIO * MAX_BLOCK_WEIGHT;
    if (recent_block) {
        // If the passed in block is populated, use to avoid a disk read.
        rebroadcast_block_weight = REBROADCAST_WEIGHT_RATIO * GetBlockWeight(*recent_block.get());
    } else {
        // Otherwise, use the block index to retrieve the relevant block.
        const Consensus::Params& consensus_params = m_chainman.GetConsensus();
        CBlock block;

        if (ReadBlockFromDisk(block, &recent_block_index, consensus_params)) {
            rebroadcast_block_weight = REBROADCAST_WEIGHT_RATIO * GetBlockWeight(block);
        }
    }

    BlockAssembler::Options options;
    options.nBlockMaxWeight = rebroadcast_block_weight;

    // Use CreateNewBlock to identify rebroadcast candidates
    std::vector<TxIds> rebroadcast_txs;
    auto block_template = BlockAssembler(m_chainman.ActiveChainstate(), &m_mempool, options)
                              .CreateNewBlock(CScript());
    rebroadcast_txs.reserve(block_template->block.vtx.size());

    for (const CTransactionRef& tx : block_template->block.vtx) {
        if (tx->IsCoinBase()) continue;

        rebroadcast_txs.push_back(TxIds(tx->GetHash(), tx->GetWitnessHash()));
    }

    return rebroadcast_txs;
};
