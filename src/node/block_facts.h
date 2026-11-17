// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_NODE_BLOCK_FACTS_H
#define BITCOIN_NODE_BLOCK_FACTS_H

#include <arith_uint256.h>
#include <chain.h>
#include <flatfile.h>
#include <sync.h>
#include <uint256.h>

#include <cstdint>

namespace node {
/**
 * What the block index records about one block, copied under cs_main in
 * one read, for a reader that then works without the lock.
 *
 * What a copy promises: the members agree with each other as of the copy.
 * What it does not: that they still hold. In particular a position may stop
 * being readable, because the file can be pruned once the lock is released;
 * a reader either handles the failed read or states why the block cannot
 * be pruned.
 */
struct BlockFacts {
    uint256 hash;
    int height{0};
    uint32_t status{0};
    unsigned int tx_count{0};
    uint64_t chain_tx_count{0};
    arith_uint256 chain_work;
    int64_t time{0};
    //! Null unless the data, respectively the undo data, is on disk.
    FlatFilePos data_pos;
    FlatFilePos undo_pos;

    bool HaveData() const { return (status & BLOCK_HAVE_DATA) != 0; }
    bool HaveUndo() const { return (status & BLOCK_HAVE_UNDO) != 0; }
};

inline BlockFacts BlockFactsOf(const CBlockIndex& block) EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
{
    AssertLockHeld(::cs_main);
    return BlockFacts{
        .hash = block.GetBlockHash(),
        .height = block.nHeight,
        .status = block.nStatus,
        .tx_count = block.nTx,
        .chain_tx_count = block.m_chain_tx_count,
        .chain_work = block.nChainWork,
        .time = block.GetBlockTime(),
        .data_pos = block.GetBlockPos(),
        .undo_pos = block.GetUndoPos(),
    };
}
} // namespace node

#endif // BITCOIN_NODE_BLOCK_FACTS_H
