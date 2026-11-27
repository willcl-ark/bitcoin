// Copyright (c) 2023-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test/util/coins.h>

#include <coins.h>
#include <coins_sharded.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <test/util/random.h>
#include <uint256.h>

#include <cstdint>
#include <utility>

template <typename CoinsViewT>
static COutPoint AddTestCoinImpl(FastRandomContext& rng, CoinsViewT& coins_view)
{
    Coin new_coin;
    COutPoint outpoint{Txid::FromUint256(rng.rand256()), /*nIn=*/0};
    new_coin.nHeight = 1;
    new_coin.out.nValue = RandMoney(rng);
    new_coin.out.scriptPubKey.assign(uint32_t{56}, 1);
    coins_view.AddCoin(outpoint, std::move(new_coin), /*possible_overwrite=*/false);

    return outpoint;
}

COutPoint AddTestCoin(FastRandomContext& rng, CCoinsViewCache& coins_view)
{
    return AddTestCoinImpl(rng, coins_view);
}

COutPoint AddTestCoin(FastRandomContext& rng, ShardedCoinsView& coins_view)
{
    return AddTestCoinImpl(rng, coins_view);
}
