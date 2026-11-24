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
constexpr size_t NUM_SHARDS{4};

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

CoinsStore::BlockChanges BuildBlockChanges(const CoinsStore& store, const SynthBlock& b, uint64_t epoch)
{
    CoinsStore::BlockChanges bc;
    bc.height = b.height;
    bc.validate_epoch = epoch;
    bc.must_be_new = false;
    bc.shard_offsets.assign(NUM_SHARDS + 1, 0);

    std::vector<CoinsStore::CommitOp> raw;
    raw.reserve(b.spends.size() + b.creates.size());
    for (uint32_t sp : b.spends) {
        const COutPoint op{OutpointAt(sp)};
        raw.push_back({
            .outpoint = op,
            .coin = Coin{},
            .key_hash = CoinsStore::KeyHashOf(op),
            .shard = store.ShardOf(op),
            .height = b.height,
        });
    }
    for (uint32_t c : b.creates) {
        const COutPoint op{OutpointAt(c)};
        raw.push_back({
            .outpoint = op,
            .coin = CoinFor(c, b.height),
            .key_hash = CoinsStore::KeyHashOf(op),
            .shard = store.ShardOf(op),
            .height = b.height,
        });
    }
    for (const auto& op : raw) ++bc.shard_offsets[op.shard + 1];
    for (size_t s{1}; s <= NUM_SHARDS; ++s) bc.shard_offsets[s] += bc.shard_offsets[s - 1];
    bc.ops.resize(raw.size());
    std::vector<uint32_t> cursor(bc.shard_offsets.begin(), bc.shard_offsets.begin() + NUM_SHARDS);
    for (auto& op : raw) {
        bc.ops[cursor[op.shard]++] = std::move(op);
    }
    return bc;
}

} // namespace

/**
 * The window oracle: exercises CoinsStore::CommitWindowBatch() with pre-bucketed per-block
 * BlockChanges across fuzzer-chosen batch sizes, intra-batch create+spend cancellation, and
 * injected double-spend conflicts that trigger exact per-block prefix rollback. The confirmed
 * state must equal a reference CCoinsViewCache at every commit and SanityCheck() must hold.
 */
FUZZ_TARGET(coinsstore_window)
{
    SeedRandomStateForTest(SeedRand::ZEROS);
    FuzzedDataProvider provider{buffer.data(), buffer.size()};

    CCoinsViewCache reference{&CoinsViewEmpty::Get()};
    CoinsStore store{&CoinsViewEmpty::Get(), /*shards=*/NUM_SHARDS, /*deterministic=*/true};
    reference.SetBestBlock(uint256::ONE);
    store.SetBestBlock(uint256::ONE, 0);

    // Seed coins.
    std::vector<uint32_t> live;
    std::vector<uint32_t> spent_history;
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
            spent_history.push_back(live[idx]);
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

    // Commit in fuzzer-chosen batches of 1..WINDOW blocks via CommitWindowBatch().
    size_t next{0};
    while (next < chain.size()) {
        const size_t max_batch{std::min(WINDOW, chain.size() - next)};
        const size_t batch_len{provider.remaining_bytes() > 0 ? provider.ConsumeIntegralInRange<size_t>(1, max_batch) : max_batch};
        const uint64_t epoch{store.CommitEpoch()};

        // Optionally inject a conflicting duplicate spend on one block in the batch to test prefix rollback.
        const bool inject_conflict{provider.remaining_bytes() > 0 && provider.ConsumeBool() && !spent_history.empty()};
        const size_t conflict_idx{inject_conflict ? provider.ConsumeIntegralInRange<size_t>(0, batch_len - 1) : batch_len};

        std::vector<CoinsStore::BlockChanges> changes;
        std::vector<CoinsStore::BlockChanges*> ptrs;
        std::vector<uint256> hashes;
        changes.reserve(batch_len);
        ptrs.reserve(batch_len);
        hashes.reserve(batch_len);

        for (size_t k{0}; k < batch_len; ++k) {
            SynthBlock b{chain[next + k]};
            if (k == conflict_idx) {
                // Append a spend of a coin that is already missing from the store (or double-spent).
                const uint32_t bad_id{spent_history[provider.ConsumeIntegralInRange<size_t>(0, spent_history.size() - 1)]};
                if (!store.PeekCoin(OutpointAt(bad_id)).has_value() &&
                    std::find(b.spends.begin(), b.spends.end(), bad_id) == b.spends.end()) {
                    b.spends.push_back(bad_id);
                }
            }
            changes.push_back(BuildBlockChanges(store, b, epoch));
            uint256 h{uint256::ONE};
            h.data()[31] = static_cast<unsigned char>(b.height);
            hashes.push_back(h);
        }
        for (auto& bc : changes) ptrs.push_back(&bc);

        const auto res{store.CommitWindowBatch(ptrs, hashes, /*pool=*/nullptr, /*publish_height=*/true)};
        assert(res.accepted <= batch_len);

        for (size_t k{0}; k < res.accepted; ++k) {
            const SynthBlock& b{chain[next + k]};
            for (uint32_t c : b.creates) reference.AddCoin(OutpointAt(c), CoinFor(c, b.height), false);
            for (uint32_t sp : b.spends) assert(reference.SpendCoin(OutpointAt(sp)));
        }
        next += res.accepted;

        for (uint32_t i{0}; i < NUM_OUTPOINTS; ++i) {
            const auto a{reference.PeekCoin(OutpointAt(i))};
            const auto b{store.PeekCoin(OutpointAt(i))};
            assert(a.has_value() == b.has_value());
            if (a) assert(a->out == b->out && a->nHeight == b->nHeight);
        }
        store.SanityCheck();
        // If res.accepted == 0 because conflict_idx == 0, the next loop iteration retries cleanly without conflict.
    }
    store.SanityCheck();
}
