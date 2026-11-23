// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <coins.h>
#include <coinsstore.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <test/fuzz/FuzzedDataProvider.h>
#include <test/fuzz/fuzz.h>
#include <test/fuzz/util.h>
#include <test/util/coinsstore.h>
#include <test/util/random.h>
#include <uint256.h>
#include <util/check.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

constexpr uint32_t NUM_OUTPOINTS{64};
constexpr size_t WINDOW{4};

COutPoint OutpointAt(uint32_t i)
{
    uint256 h;
    h.data()[0] = static_cast<unsigned char>(i);
    h.data()[1] = 0x55;
    return COutPoint{Txid::FromUint256(h), i % 3};
}

Coin CoinFor(uint32_t i, int height)
{
    return Coin{CTxOut{static_cast<CAmount>(1000 + i), CScript() << std::vector<unsigned char>{static_cast<unsigned char>(i)}}, height, false};
}

struct SynthBlock {
    int height{0};
    std::vector<uint32_t> spends;
    std::vector<uint32_t> creates;
};

} // namespace

/**
 * The window oracle: a valid chain of synthetic blocks is validated read-only against CoinsStore
 * plus WindowOverlay (with intra-batch create+spend cancellation) and committed in exact height
 * order via CommitBatch() in fuzzer-chosen batch sizes. The confirmed state must equal a
 * reference CCoinsViewCache at every commit and CoinsStore::SanityCheck() must hold.
 */
FUZZ_TARGET(coinsstore_window)
{
    SeedRandomStateForTest(SeedRand::ZEROS);
    FuzzedDataProvider provider{buffer.data(), buffer.size()};

    CCoinsViewCache reference{&CoinsViewEmpty::Get()};
    CoinsStore store{&CoinsViewEmpty::Get(), /*shards=*/4, /*deterministic=*/true};
    reference.SetBestBlock(uint256::ONE);
    store.SetBestBlock(uint256::ONE, 0);

    // Seed coins.
    std::vector<uint32_t> live;
    const uint32_t seeded{provider.ConsumeIntegralInRange<uint32_t>(1, 16)};
    for (uint32_t i{0}; i < seeded; ++i) {
        reference.AddCoin(OutpointAt(i), CoinFor(i, 0), false);
        store.AddCoin(OutpointAt(i), CoinFor(i, 0), false);
        live.push_back(i);
    }
    uint32_t next_fresh{seeded};

    // Build a valid chain in order.
    std::vector<SynthBlock> chain;
    const int blocks{provider.ConsumeIntegralInRange<int>(1, 16)};
    for (int h{1}; h <= blocks; ++h) {
        SynthBlock b{.height = h, .spends = {}, .creates = {}};
        const int spends{provider.ConsumeIntegralInRange<int>(0, 3)};
        for (int k{0}; k < spends && !live.empty(); ++k) {
            const size_t idx{provider.ConsumeIntegralInRange<size_t>(0, live.size() - 1)};
            b.spends.push_back(live[idx]);
            live.erase(live.begin() + idx);
        }
        const int creates{provider.ConsumeIntegralInRange<int>(0, 3)};
        for (int k{0}; k < creates && next_fresh < NUM_OUTPOINTS; ++k) {
            b.creates.push_back(next_fresh);
            live.push_back(next_fresh);
            ++next_fresh;
        }
        chain.push_back(std::move(b));
    }

    // Commit in fuzzer-chosen batches of 1..WINDOW blocks with intra-batch cancellation.
    size_t next{0};
    while (next < chain.size()) {
        const size_t max_batch{std::min(WINDOW, chain.size() - next)};
        const size_t batch_len{provider.remaining_bytes() > 0 ? provider.ConsumeIntegralInRange<size_t>(1, max_batch) : max_batch};

        std::unordered_map<COutPoint, size_t, SaltedOutpointHasher> batch_live_creates;
        std::vector<CoinsStore::CommitOp> ops;
        int last_h{0};
        for (size_t k{0}; k < batch_len; ++k) {
            const SynthBlock& b{chain[next + k]};
            last_h = b.height;
            for (uint32_t sp : b.spends) {
                const COutPoint op{OutpointAt(sp)};
                if (const auto it{batch_live_creates.find(op)}; it != batch_live_creates.end()) {
                    ops[it->second].cancelled = true;
                    batch_live_creates.erase(it);
                } else {
                    assert(store.PeekCoin(op).has_value());
                    ops.push_back({.outpoint = op, .coin = Coin{}, .key_hash = CoinsStore::KeyHashOf(op), .shard = store.ShardOf(op), .height = b.height});
                }
            }
            for (uint32_t c : b.creates) {
                const COutPoint op{OutpointAt(c)};
                batch_live_creates.emplace(op, ops.size());
                ops.push_back({.outpoint = op, .coin = CoinFor(c, b.height), .key_hash = CoinsStore::KeyHashOf(op), .shard = store.ShardOf(op), .height = b.height});
            }
        }
        uint256 best_hash{uint256::ONE};
        best_hash.data()[31] = static_cast<unsigned char>(last_h);
        store.CommitBatch(ops, best_hash, last_h);

        for (size_t k{0}; k < batch_len; ++k) {
            const SynthBlock& b{chain[next + k]};
            for (uint32_t c : b.creates) reference.AddCoin(OutpointAt(c), CoinFor(c, b.height), false);
            for (uint32_t sp : b.spends) assert(reference.SpendCoin(OutpointAt(sp)));
        }
        next += batch_len;

        for (uint32_t i{0}; i < NUM_OUTPOINTS; ++i) {
            const auto a{reference.PeekCoin(OutpointAt(i))};
            const auto b{store.PeekCoin(OutpointAt(i))};
            assert(a.has_value() == b.has_value());
            if (a) assert(a->out == b->out && a->nHeight == b->nHeight);
        }
        store.SanityCheck();
    }
    store.SanityCheck();
}
