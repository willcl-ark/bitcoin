// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_TEST_UTIL_COINSSTORE_H
#define BITCOIN_TEST_UTIL_COINSSTORE_H

#include <coins.h>
#include <coinsstore.h>
#include <primitives/transaction.h>

#include <utility>
#include <vector>

//! A change that owns its outpoint and its coin, as a test writes one down (CoinsStore::Change points at
//! a view's entry).
struct OwnedChange {
    COutPoint outpoint;
    Coin coin;
    bool possible_overwrite{false};
    bool must_be_new{false};
    bool known_coin{false};
};

//! CoinsStore::ApplyBatch() of `changes`.
inline void ApplyBatch(CoinsStore& store, CoinsStore::BlockId block, std::vector<OwnedChange>&& changes)
{
    std::vector<CoinsStore::Change> refs;
    refs.reserve(changes.size());
    for (OwnedChange& change : changes) {
        refs.push_back({.outpoint = &change.outpoint, .coin = &change.coin, .possible_overwrite = change.possible_overwrite, .must_be_new = change.must_be_new, .known_coin = change.known_coin});
    }
    store.ApplyBatch(block, std::move(refs));
}

#endif // BITCOIN_TEST_UTIL_COINSSTORE_H
