// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/connect_window.h>

#include <memusage.h>
#include <util/check.h>
#include <util/hasher.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <optional>
#include <unordered_set>
#include <utility>
#include <vector>

namespace node {

namespace {

constexpr size_t PARSED_BYTES_PER_TX{256};
constexpr size_t PARSED_BYTES_PER_INPUT{96};
constexpr size_t PARSED_BYTES_PER_OUTPUT{64};

void CheckGroup(CommitJob& job, size_t g)
{
    const uint32_t count{static_cast<uint32_t>(job.blocks.size())};
    const CoinsStore& store{*job.store};
    const size_t shards{store.ShardCount()};
    const size_t num_groups{job.num_groups};
    const size_t shard_begin{g * shards / num_groups};
    const size_t shard_end{(g + 1) * shards / num_groups};
    const size_t local_shards{shard_end - shard_begin};

    CommitJob::GroupData& gd{job.group_data[g]};
    gd.shard_begin = shard_begin;
    gd.shard_end = shard_end;

    size_t total_items{0};
    for (uint32_t b{0}; b < count; ++b) {
        const StagedConnect& s{*job.blocks[b]};
        total_items += s.group_starts[g + 1] - s.group_starts[g];
    }
    if (total_items == 0 || local_shards == 0) return;

    // Bucket items across blocks 0..count-1 by shard (preserving block order within each shard).
    // The resulting GroupData is verified in Phase 1 and reused verbatim in Phase 2.
    gd.shard_starts.assign(local_shards + 1, 0);
    std::vector<uint32_t> item_shard;
    item_shard.reserve(total_items);
    for (uint32_t b{0}; b < count; ++b) {
        const StagedConnect& s{*job.blocks[b]};
        for (uint32_t i{s.group_starts[g]}; i < s.group_starts[g + 1]; ++i) {
            const size_t s_idx{store.ShardIndexOf(s.items[i].Outpoint())};
            Assert(s_idx >= shard_begin && s_idx < shard_end);
            const uint32_t local{static_cast<uint32_t>(s_idx - shard_begin)};
            item_shard.push_back(local);
            ++gd.shard_starts[local + 1];
        }
    }
    for (size_t ls{0}; ls < local_shards; ++ls) gd.shard_starts[ls + 1] += gd.shard_starts[ls];

    gd.ops.resize(total_items);
    std::vector<uint32_t> next(gd.shard_starts.begin(), gd.shard_starts.end() - 1);
    size_t flat{0};
    for (uint32_t b{0}; b < count; ++b) {
        const StagedConnect& s{*job.blocks[b]};
        const bool same_gen{s.resolve_flush_gen == job.store_flush_gen};
        const uint32_t height{static_cast<uint32_t>(job.heights[b])};
        for (uint32_t i{s.group_starts[g]}; i < s.group_starts[g + 1]; ++i) {
            const uint32_t slot{next[item_shard[flat++]]++};
            const CommitItem& item{s.items[i]};
            const COutPoint outpoint{item.Outpoint()};
            const size_t hash{store.HashOutpoint(outpoint)};
            if (item.Spend()) {
                gd.ops[slot] = {
                    .outpoint = outpoint,
                    .create_out = nullptr,
                    .code = item.n,
                    .block_idx = b,
                    .hash = hash,
                    .possible_overwrite = false,
                    .trust_from_base = same_gen && (item.flags & CommitItem::FROM_BASE) != 0,
                };
            } else {
                const CTransaction& tx{item.Tx()};
                gd.ops[slot] = {
                    .outpoint = outpoint,
                    .create_out = &tx.vout[item.n],
                    .code = (height << 1) | uint32_t{tx.IsCoinBase()},
                    .block_idx = b,
                    .hash = hash,
                    .possible_overwrite = (item.flags & CommitItem::OVERWRITE) != 0,
                    .trust_from_base = false,
                };
            }
        }
    }

    uint32_t earliest_conflict{CommitJob::NO_CONFLICT};
    for (size_t ls{0}; ls < local_shards; ++ls) {
        const uint32_t begin{gd.shard_starts[ls]};
        const uint32_t end{gd.shard_starts[ls + 1]};
        if (begin == end) continue;
        const uint32_t conflict{store.CheckShard(
            shard_begin + ls,
            std::span<const CoinsStore::ShardOp>{gd.ops}.subspan(begin, end - begin),
            CommitJob::NO_CONFLICT)};
        earliest_conflict = std::min(earliest_conflict, conflict);
    }

    if (earliest_conflict != CommitJob::NO_CONFLICT) {
        uint32_t first{job.first_conflict.load(std::memory_order_relaxed)};
        while (earliest_conflict < first &&
               !job.first_conflict.compare_exchange_weak(first, earliest_conflict, std::memory_order_relaxed)) {}
    }
}

void ApplyGroup(CommitJob& job, size_t g, uint32_t valid_blocks)
{
    CommitJob::GroupData& gd{job.group_data[g]};
    if (gd.ops.empty() || valid_blocks == 0) return;
    CoinsStore& store{*job.store};
    const size_t local_shards{gd.shard_end - gd.shard_begin};
    for (size_t ls{0}; ls < local_shards; ++ls) {
        const uint32_t begin{gd.shard_starts[ls]};
        const uint32_t end{gd.shard_starts[ls + 1]};
        if (begin == end) continue;
        store.ApplyShardUnconditional(
            gd.shard_begin + ls,
            std::span<const CoinsStore::ShardOp>{gd.ops}.subspan(begin, end - begin),
            valid_blocks);
    }
}

} // namespace

size_t ParsedBlockBytes(const CBlock& block, size_t serialized, size_t& inputs)
{
    size_t outputs{0};
    inputs = 0;
    for (const auto& tx : block.vtx) {
        inputs += tx->vin.size();
        outputs += tx->vout.size();
    }
    if (serialized == 0) {
        return sizeof(CBlock) + (block.vtx.size() + inputs) * WINDOW_BYTES_PER_ITEM;
    }
    return sizeof(CBlock) + serialized + block.vtx.size() * PARSED_BYTES_PER_TX +
           inputs * PARSED_BYTES_PER_INPUT + outputs * PARSED_BYTES_PER_OUTPUT;
}

size_t WindowBlockBytes(const CBlock& block, int num_inputs, size_t block_bytes)
{
    const size_t by_counts{(block.vtx.size() + static_cast<size_t>(std::max(0, num_inputs))) * WINDOW_BYTES_PER_ITEM};
    return std::max(by_counts, block_bytes);
}

void ResolveWindowInputs(const CoinsStore& store,
                         const WindowOutputs& outputs,
                         int viewer_height,
                         const CBlock& block,
                         std::vector<Coin>& coins,
                         std::vector<uint8_t>& from_base)
{
    size_t total{0};
    for (size_t t{1}; t < block.vtx.size(); ++t) total += block.vtx[t]->vin.size();
    coins.assign(total, Coin{});
    from_base.assign(total, 0);
    if (total == 0) return;

    std::vector<COutPoint> outpoints;
    std::vector<uint32_t> positions;
    outpoints.reserve(total);
    positions.reserve(total);
    for (size_t t{1}; t < block.vtx.size(); ++t) {
        for (const CTxIn& in : block.vtx[t]->vin) {
            outpoints.push_back(in.prevout);
            positions.push_back(static_cast<uint32_t>(t));
        }
    }

    // 1. Check earlier transactions in this block and lower unpublished blocks in the window.
    std::vector<uint8_t> decided(total, 0);
    outputs.FindInputs(outpoints, positions, viewer_height, decided, coins);

    // 2. Check CoinsStore in a sharded batch (3-state: absent, found, or spent tombstone).
    std::vector<uint32_t> need_store;
    need_store.reserve(total);
    for (uint32_t i{0}; i < total; ++i) {
        if (!decided[i]) need_store.push_back(i);
    }
    if (need_store.empty()) return;

    std::vector<uint8_t> store_state;
    store.FetchInto(outpoints, need_store, coins, store_state);

    // 3. Query LevelDB directly ONLY for FETCH_ABSENT misses, sorted by key to localize SSTable reads.
    std::vector<uint32_t> need_base;
    for (const uint32_t w : need_store) {
        if (store_state[w] == CoinsStore::FETCH_ABSENT) {
            need_base.push_back(w);
        }
    }
    if (need_base.size() > 1) {
        std::sort(need_base.begin(), need_base.end(), [&](uint32_t a, uint32_t b) {
            return outpoints[a] < outpoints[b];
        });
    }
    for (const uint32_t w : need_base) {
        if (auto c{store.BaseCoin(outpoints[w])}) {
            coins[w] = std::move(*c);
            from_base[w] = 1;
        }
    }
}

bool WindowHaveAnyOutput(const CoinsStore& store,
                         const WindowOutputs& outputs,
                         int viewer_height,
                         const CBlock& block)
{
    std::vector<uint8_t> named;
    outputs.NamedIds(block.vtx, viewer_height, named);
    for (size_t i{0}; i < block.vtx.size(); ++i) {
        if (!named[i]) continue;
        const CTransaction& tx{*block.vtx[i]};
        for (uint32_t o{0}; o < tx.vout.size(); ++o) {
            std::optional<Coin> coin;
            if (outputs.FindCoin(COutPoint{tx.GetHash(), o}, viewer_height, coin) && coin) {
                return true;
            }
        }
    }

    size_t total_outs{0};
    for (const auto& tx : block.vtx) total_outs += tx->vout.size();
    std::vector<COutPoint> outpoints;
    outpoints.reserve(total_outs);
    for (const auto& tx : block.vtx) {
        const Txid& txid{tx->GetHash()};
        for (uint32_t o{0}; o < tx->vout.size(); ++o) {
            outpoints.emplace_back(txid, o);
        }
    }
    std::vector<uint8_t> may;
    store.MayHave(outpoints, may);
    return std::ranges::any_of(may, [](uint8_t v) { return v != 0; });
}

void PrepareCommit(StagedConnect& s, const CBlock& block, const CBlockUndo& undo, std::span<const uint8_t> from_base)
{
    const CoinsStore& store{*Assert(s.store)};
    const size_t shards{store.ShardCount()};
    const size_t num_groups{std::min(COMMIT_GROUPS, shards)};
    if (!s.pindex->pprev) {
        s.group_starts.assign(num_groups + 1, 0);
        return;
    }

    const uint32_t own_height{static_cast<uint32_t>(s.pindex->nHeight)};
    static_assert(COMMIT_GROUPS < 0xFF);
    static constexpr uint8_t SKIP{0xFF};

    std::vector<uint8_t> groups;
    s.group_starts.assign(num_groups + 1, 0);
    const auto group_of{[&](const COutPoint& outpoint) {
        const uint8_t g{static_cast<uint8_t>(store.ShardIndexOf(outpoint) * num_groups / shards)};
        ++s.group_starts[g + 1];
        return g;
    }};

    // Lazy-construct own_spent only when a block spends an output created by an earlier tx in the same block.
    std::optional<std::unordered_set<COutPoint, SaltedCoinsCacheHasher>> own_spent;
    Assert(undo.vtxundo.size() + 1 == block.vtx.size());
    for (size_t t{1}; t < block.vtx.size(); ++t) {
        const CTransaction& tx{*block.vtx[t]};
        const CTxUndo& tx_undo{undo.vtxundo[t - 1]};
        Assert(tx_undo.vprevout.size() == tx.vin.size());
        for (size_t j{0}; j < tx.vin.size(); ++j) {
            if (tx_undo.vprevout[j].nHeight == own_height) {
                if (!own_spent) own_spent.emplace();
                own_spent->insert(tx.vin[j].prevout);
                continue;
            }
            groups.push_back(group_of(tx.vin[j].prevout));
        }
    }

    for (size_t t{0}; t < block.vtx.size(); ++t) {
        const CTransaction& tx{*block.vtx[t]};
        for (uint32_t n{0}; n < tx.vout.size(); ++n) {
            if (tx.vout[n].scriptPubKey.IsUnspendable()) continue;
            const COutPoint outpoint{tx.GetHash(), n};
            const bool spent_here{own_spent && own_spent->contains(outpoint)};
            groups.push_back(spent_here ? SKIP : group_of(outpoint));
        }
    }

    for (size_t g{0}; g < num_groups; ++g) s.group_starts[g + 1] += s.group_starts[g];
    std::array<uint32_t, COMMIT_GROUPS> next{};
    std::copy(s.group_starts.begin(), s.group_starts.end() - 1, next.begin());
    s.items.resize(s.group_starts[num_groups]);
    size_t k{0};
    size_t flat_in{0};
    for (size_t t{1}; t < block.vtx.size(); ++t) {
        const CTransaction& tx{*block.vtx[t]};
        const CTxUndo& tx_undo{undo.vtxundo[t - 1]};
        for (size_t j{0}; j < tx.vin.size(); ++j) {
            const bool base_read{flat_in < from_base.size() && from_base[flat_in] != 0};
            ++flat_in;
            const Coin& coin{tx_undo.vprevout[j]};
            if (coin.nHeight == own_height) continue;
            const uint8_t flags{static_cast<uint8_t>(CommitItem::SPEND | (base_read ? CommitItem::FROM_BASE : 0))};
            s.items[next[groups[k++]]++] = {
                .at = &tx.vin[j].prevout,
                .n = (uint32_t{coin.nHeight} << 1) | uint32_t{coin.fCoinBase},
                .flags = flags,
            };
        }
    }
    for (size_t t{0}; t < block.vtx.size(); ++t) {
        const CTransaction& tx{*block.vtx[t]};
        const uint8_t flags{t == 0 ? CommitItem::OVERWRITE : uint8_t{0}};
        for (uint32_t n{0}; n < tx.vout.size(); ++n) {
            if (tx.vout[n].scriptPubKey.IsUnspendable()) continue;
            const uint8_t g{groups[k++]};
            if (g == SKIP) continue;
            s.items[next[g]++] = {.at = &tx, .n = n, .flags = flags};
        }
    }
    Assert(k == groups.size());
}

std::shared_ptr<CommitJob> StartCommitJob(WindowSync& sync, StagedConnect& front, size_t max_blocks, int max_height)
{
    AssertLockHeld(sync.mutex);
    Assert(!sync.active_job && !front.committed && !front.in_job);
    auto job{std::make_shared<CommitJob>()};
    job->store = Assert(front.store);
    job->num_groups = std::min(COMMIT_GROUPS, job->store->ShardCount());
    job->store_flush_gen = job->store->FlushGeneration();
    job->blocks.push_back(&front);
    for (StagedConnect* s : sync.slots) {
        if (job->blocks.size() >= max_blocks || s->pindex->nHeight > max_height) break;
        if (!IsWindowComplete(s->window) || !s->ok || s->read_failed) break;
        job->blocks.push_back(s);
    }
    for (StagedConnect* s : job->blocks) {
        s->in_job = true;
        job->items += s->items.size();
        job->heights.push_back(s->pindex->nHeight);
    }
    sync.active_job = job;
    sync.job_open.store(true, std::memory_order_release);
    return job;
}

void RunCommitWork(WindowSync& sync) noexcept
{
    const std::shared_ptr<CommitJob> job{WITH_LOCK(sync.mutex, return sync.active_job)};
    if (!job) return;
    const size_t num_groups{job->num_groups};

    // Phase 1: check every shard group read-only against CoinsStore.
    if (job->checks_done.load(std::memory_order_acquire) < num_groups) {
        for (size_t g{0}; g < num_groups; ++g) {
            if (job->check_state[g].load(std::memory_order_relaxed) != 0) continue;
            uint8_t expected{0};
            if (!job->check_state[g].compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) continue;
            try {
                CheckGroup(*job, g);
            } catch (const std::exception&) {
                LOCK(job->error_mutex);
                if (!job->error) job->error = std::current_exception();
                job->first_conflict.store(0, std::memory_order_relaxed);
            }
            job->check_state[g].store(2, std::memory_order_release);
            if (job->checks_done.fetch_add(1, std::memory_order_acq_rel) + 1 == num_groups) {
                const bool has_err{WITH_LOCK(job->error_mutex, return job->error != nullptr)};
                const uint32_t valid{has_err
                                         ? 0U
                                         : std::min<uint32_t>(static_cast<uint32_t>(job->blocks.size()),
                                                              job->first_conflict.load(std::memory_order_relaxed))};
                {
                    LOCK(sync.mutex);
                    job->valid_blocks.store(valid, std::memory_order_release);
                    job->phase2_ready.store(true, std::memory_order_release);
                }
                sync.cv.notify_all();
            }
        }
    }

    // Barrier: wait until every Phase 1 check has completed before mutating CoinsStore.
    if (!job->phase2_ready.load(std::memory_order_acquire)) {
        WAIT_LOCK(sync.mutex, lock);
        sync.cv.wait(lock, [&]() { return job->phase2_ready.load(std::memory_order_acquire); });
    }

    // Phase 2: apply verified blocks 0..valid_blocks-1 unconditionally.
    const uint32_t valid_blocks{job->valid_blocks.load(std::memory_order_acquire)};
    if (valid_blocks > 0 && job->applies_done.load(std::memory_order_acquire) < num_groups) {
        for (size_t g{0}; g < num_groups; ++g) {
            if (job->apply_state[g].load(std::memory_order_relaxed) != 0) continue;
            uint8_t expected{0};
            if (!job->apply_state[g].compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) continue;
            ApplyGroup(*job, g, valid_blocks);
            job->apply_state[g].store(2, std::memory_order_release);
            job->applies_done.fetch_add(1, std::memory_order_acq_rel);
        }
    }

    if (valid_blocks > 0 && job->applies_done.load(std::memory_order_acquire) < num_groups) return;
    if (job->finalizing.exchange(true, std::memory_order_acq_rel)) return;

    sync.job_open.store(false, std::memory_order_release);
    {
        LOCK(sync.mutex);
        job->ready = true;
        if (sync.active_job == job) {
            sync.active_job.reset();
            for (uint32_t b{0}; b < job->blocks.size(); ++b) {
                StagedConnect* const s{job->blocks[b]};
                if (b < valid_blocks) {
                    s->committed = true;
                } else {
                    s->commit_failed = true;
                }
            }
        }
    }
    sync.cv.notify_all();
}

void DrainWindow(std::shared_ptr<WindowSync>& sync,
                 std::deque<std::unique_ptr<StagedConnect>>& in_flight)
{
    if (sync) {
        LOCK(sync->mutex);
        sync->draining = true;
        Assert(!sync->active_job);
    }
    for (auto it{in_flight.rbegin()}; it != in_flight.rend(); ++it) {
        StagedConnect& staged{**it};
        if (staged.done.valid()) staged.done.wait();
        {
            WAIT_LOCK(staged.sync->mutex, lock);
            staged.sync->cv.wait(lock, [&]() EXCLUSIVE_LOCKS_REQUIRED(staged.sync->mutex) {
                return staged.window != WindowBlockState::Validating;
            });
            std::erase(staged.sync->slots, &staged);
        }
    }
    in_flight.clear();
}

} // namespace node
