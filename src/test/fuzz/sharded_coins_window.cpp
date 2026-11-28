// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <coins.h>
#include <coins_sharded.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <test/fuzz/FuzzedDataProvider.h>
#include <test/fuzz/fuzz.h>
#include <test/fuzz/util.h>
#include <test/util/random.h>
#include <uint256.h>
#include <util/check.h>

#include <cstdint>
#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace {

constexpr uint32_t NUM_OUTPOINTS{48};

struct OutPointOrder {
    bool operator()(const COutPoint& a, const COutPoint& b) const noexcept
    {
        return CompareOutPointsLevelDBOrder(a, b);
    }
};

class FuzzBackingView final : public CCoinsView
{
public:
    std::map<COutPoint, Coin, OutPointOrder> db;
    uint256 best_block;

    std::optional<Coin> GetCoin(const COutPoint& op) const override
    {
        auto it = db.find(op);
        return it == db.end() ? std::nullopt : std::optional<Coin>{it->second};
    }
    std::optional<Coin> PeekCoin(const COutPoint& op) const override { return GetCoin(op); }
    bool HaveCoin(const COutPoint& op) const override { return db.contains(op); }
    uint256 GetBestBlock() const override { return best_block; }
    std::vector<uint256> GetHeadBlocks() const override { return {}; }
    size_t EstimateSize() const override { return db.size(); }

    void BatchWrite(CoinsViewCacheCursor& cursor, const uint256& block_hash) override
    {
        BatchWriteEntries(cursor.GetDirtyCount(), cursor.GetTotalCount(), block_hash, [&](CoinsWriteSink& sink) {
            for (auto it = cursor.Begin(); it != cursor.End(); it = cursor.NextAndMaybeErase(*it)) {
                if (!it->second.IsDirty()) continue;
                if (it->second.coin.IsSpent()) {
                    sink.Erase(it->first);
                } else {
                    sink.Write(it->first, it->second.coin);
                }
            }
        });
    }

    void BatchWriteEntries(
        size_t,
        size_t,
        const uint256& block_hash,
        const std::function<void(CoinsWriteSink&)>& emit_entries) override
    {
        struct Sink final : CoinsWriteSink {
            FuzzBackingView& self;
            explicit Sink(FuzzBackingView& s) : self{s} {}
            void Write(const COutPoint& op, const Coin& coin) override { self.db[op] = coin; }
            void Erase(const COutPoint& op) override { self.db.erase(op); }
        } sink{*this};
        emit_entries(sink);
        best_block = block_hash;
    }
};

COutPoint MakeOutPoint(uint32_t idx)
{
    uint256 h;
    h.SetNull();
    h.data()[0] = static_cast<unsigned char>((idx * 19) & 0xFF);
    h.data()[1] = static_cast<unsigned char>(idx & 0xFF);
    return COutPoint{Txid::FromUint256(h), idx % 3};
}

Coin MakeCoin(FuzzedDataProvider& provider, int height)
{
    const CAmount val = static_cast<CAmount>(provider.ConsumeIntegralInRange<uint32_t>(0, 1'000'000));
    const bool cb = provider.ConsumeBool();
    const uint8_t kind = provider.ConsumeIntegralInRange<uint8_t>(0, 3);
    CScript script;
    if (kind == 0) {
        script = CScript() << OP_TRUE;
    } else if (kind == 1) {
        script = CScript() << OP_0 << std::vector<unsigned char>(20, static_cast<unsigned char>(height & 0xFF));
    } else if (kind == 2) {
        script = CScript() << OP_1 << std::vector<unsigned char>(32, static_cast<unsigned char>(height & 0xFF));
    } else {
        script = CScript() << std::vector<unsigned char>(40, static_cast<unsigned char>(height & 0xFF)) << OP_DROP << OP_TRUE;
    }
    return Coin{CTxOut{val, std::move(script)}, height, cb};
}

} // namespace

/**
 * Differential oracle fuzz target comparing ShardedCoinsView against CCoinsViewCache
 * across AddCoin, SpendCoin, GetCoin, Uncache, BatchWrite, BatchWriteEntries, Sync,
 * Flush, CommitBlockBatch (including partial rollback on conflicts), and batch lookups.
 */
FUZZ_TARGET(sharded_coins_window)
{
    SeedRandomStateForTest(SeedRand::ZEROS);
    FuzzedDataProvider provider{buffer.data(), buffer.size()};

    FuzzBackingView base_sharded;
    FuzzBackingView base_ref;
    ShardedCoinsView sharded{&base_sharded, /*deterministic=*/true};
    CCoinsViewCache ref{&base_ref, /*deterministic=*/true};

    uint256 tip{uint256::ONE};
    sharded.SetBestBlock(tip);
    ref.SetBestBlock(tip);

    int height{1};
    LIMITED_WHILE(provider.remaining_bytes() > 0, 500)
    {
        const uint8_t op_kind = provider.ConsumeIntegralInRange<uint8_t>(0, 9);
        const uint32_t idx = provider.ConsumeIntegralInRange<uint32_t>(0, NUM_OUTPOINTS - 1);
        const COutPoint op = MakeOutPoint(idx);

        switch (op_kind) {
        case 0: { // AddCoin
            const bool exists = ref.HaveCoin(op);
            const bool overwrite = exists || provider.ConsumeBool();
            Coin c = MakeCoin(provider, height);
            sharded.AddCoin(op, Coin{c}, overwrite);
            ref.AddCoin(op, Coin{c}, overwrite);
            break;
        }
        case 1: { // SpendCoin
            const bool had_coin = ref.HaveCoin(op);
            Coin ms, mr;
            const bool rs = sharded.SpendCoin(op, &ms);
            const bool rr = ref.SpendCoin(op, &mr);
            Assert(rs == had_coin);
            if (had_coin) {
                Assert(rr);
                Assert(ms.out == mr.out && ms.nHeight == mr.nHeight && ms.fCoinBase == mr.fCoinBase);
            } else {
                Assert(!rr || mr.IsSpent());
            }
            break;
        }
        case 2: { // GetCoin + PeekCoin
            const auto cs = sharded.GetCoin(op);
            const auto cr = ref.GetCoin(op);
            Assert(cs.has_value() == cr.has_value());
            if (cs) Assert(cs->out == cr->out && cs->nHeight == cr->nHeight && cs->fCoinBase == cr->fCoinBase);
            break;
        }
        case 3: { // Uncache
            sharded.Uncache(op);
            ref.Uncache(op);
            break;
        }
        case 4: { // BatchWrite via overlay
            CCoinsViewCache os{&sharded};
            CCoinsViewCache or_{&ref};
            if (or_.HaveCoin(op)) {
                Assert(os.SpendCoin(op) == or_.SpendCoin(op));
            } else {
                Coin c = MakeCoin(provider, height);
                os.AddCoin(op, Coin{c}, false);
                or_.AddCoin(op, Coin{c}, false);
            }
            tip.data()[0] ^= static_cast<unsigned char>(height & 0xFF);
            os.SetBestBlock(tip);
            or_.SetBestBlock(tip);
            os.Flush();
            or_.Flush();
            ++height;
            break;
        }
        case 5: { // BatchWriteEntries
            const bool do_write = provider.ConsumeBool();
            Coin c = MakeCoin(provider, height);
            tip.data()[1] ^= static_cast<unsigned char>(height & 0xFF);
            auto emit = [&](CoinsWriteSink& sink) {
                if (do_write) sink.Write(op, c);
                else sink.Erase(op);
            };
            sharded.BatchWriteEntries(1, 1, tip, emit);
            ref.BatchWriteEntries(1, 1, tip, emit);
            ++height;
            break;
        }
        case 6: { // CommitBlockBatch (2 blocks, second optionally conflicts and rolls back)
            uint256 b0 = tip;
            b0.data()[2] = static_cast<unsigned char>((height) & 0xFF);
            uint256 b1 = tip;
            b1.data()[2] = static_cast<unsigned char>((height + 1) & 0xFF);
            const uint64_t epoch = sharded.FlushEpoch();

            std::vector<ShardedCoinsView::BatchCommitOp> batch_ops;
            std::optional<COutPoint> b0_spent;
            std::optional<std::pair<COutPoint, Coin>> b0_created;

            if (ref.HaveCoin(op)) {
                const bool from_db = !sharded.HaveCoinInCache(op);
                batch_ops.push_back({op, Coin{}, 0, ShardedCoinsView::BatchCommitOp::Kind::SPEND, from_db, false});
                b0_spent = op;
            }
            const uint32_t idx2 = provider.ConsumeIntegralInRange<uint32_t>(0, NUM_OUTPOINTS - 1);
            const COutPoint op2 = MakeOutPoint(idx2);
            if (op2 != op && !ref.HaveCoin(op2)) {
                Coin c = MakeCoin(provider, height);
                batch_ops.push_back({op2, c, 0, ShardedCoinsView::BatchCommitOp::Kind::CREATE, false, false});
                b0_created = {op2, c};
            }
            const bool force_conflict = provider.ConsumeBool();
            if (force_conflict) {
                const COutPoint missing = MakeOutPoint(NUM_OUTPOINTS + 5);
                batch_ops.push_back({missing, Coin{}, 1, ShardedCoinsView::BatchCommitOp::Kind::SPEND, false, false});
            }
            std::vector<ShardedCoinsView::BatchCommitBlockMeta> meta{{b0, epoch, static_cast<uint32_t>(height), false}, {b1, epoch, static_cast<uint32_t>(height + 1), false}};
            std::array<std::vector<ShardedCoinsView::BatchCommitOp>, ShardedCoinsView::NUM_SHARDS> shard_ops;
            for (const auto& bop : batch_ops) {
                shard_ops[ShardedCoinsView::ShardIndex(bop.outpoint)].push_back(bop);
            }
            const size_t committed = sharded.CommitBlockBatch(meta, shard_ops);
            Assert(committed == (force_conflict ? 1U : 2U));

            if (b0_spent) Assert(ref.SpendCoin(*b0_spent));
            if (b0_created) ref.AddCoin(b0_created->first, Coin{b0_created->second}, false);
            tip = force_conflict ? b0 : b1;
            ref.SetBestBlock(tip);
            ++height;
            break;
        }
        case 7: { // Sync
            sharded.Sync();
            ref.Sync();
            Assert(base_sharded.db.size() == base_ref.db.size());
            break;
        }
        case 8: { // FlushPartial
            const size_t target_bytes = provider.ConsumeIntegralInRange<size_t>(0, 65536);
            sharded.FlushPartial(target_bytes);
            ref.Sync();
            Assert(base_sharded.db.size() == base_ref.db.size());
            Assert(sharded.DynamicMemoryUsage() <= target_bytes);
            break;
        }
        default: { // Flush
            const bool realloc_cache = provider.ConsumeBool();
            sharded.Flush(realloc_cache);
            ref.Flush(realloc_cache);
            Assert(base_sharded.db.size() == base_ref.db.size());
            break;
        }
        }
    }

    // Verify batch lookup equivalence and internal invariants.
    std::vector<COutPoint> all_ops;
    all_ops.reserve(NUM_OUTPOINTS);
    for (uint32_t i = 0; i < NUM_OUTPOINTS; ++i) all_ops.push_back(MakeOutPoint(i));
    std::vector<ShardedCoinsView::BatchLookupResult> batch_res(NUM_OUTPOINTS);
    sharded.LookupBatch(all_ops, batch_res);
    Assert(batch_res.size() == NUM_OUTPOINTS);

    for (uint32_t i = 0; i < NUM_OUTPOINTS; ++i) {
        const auto a = sharded.PeekCoin(all_ops[i]);
        const auto b = ref.PeekCoin(all_ops[i]);
        Assert(a.has_value() == b.has_value());
        if (a) Assert(a->out == b->out && a->nHeight == b->nHeight && a->fCoinBase == b->fCoinBase);
    }
    sharded.SanityCheck();
}
