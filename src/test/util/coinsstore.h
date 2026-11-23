// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_TEST_UTIL_COINSSTORE_H
#define BITCOIN_TEST_UTIL_COINSSTORE_H

#include <coins.h>
#include <coinsstore.h>
#include <primitives/transaction.h>
#include <uint256.h>

#include <utility>
#include <vector>

//! A change that owns its outpoint and its coin, as a test writes one down.
struct OwnedChange {
    COutPoint outpoint;
    Coin coin;
    int height{0};
    bool possible_overwrite{false};
};

//! Build a CommitOp vector from `changes` and commit it in order via CoinsStore::CommitBatch().
inline void CommitChanges(CoinsStore& store, std::vector<OwnedChange>&& changes, const uint256& best_hash, int best_height, ThreadPool* pool = nullptr)
{
    std::vector<CoinsStore::CommitOp> ops;
    ops.reserve(changes.size());
    for (OwnedChange& change : changes) {
        ops.push_back({
            .outpoint = change.outpoint,
            .coin = std::move(change.coin),
            .key_hash = CoinsStore::KeyHashOf(change.outpoint),
            .shard = store.ShardOf(change.outpoint),
            .height = change.height,
            .possible_overwrite = change.possible_overwrite,
            .cancelled = false,
        });
    }
    store.CommitBatch(ops, best_hash, best_height, pool);
}

#endif // BITCOIN_TEST_UTIL_COINSSTORE_H
