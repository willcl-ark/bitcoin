// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/connect_window.h>

#include <memusage.h>
#include <util/check.h>
#include <util/hasher.h>
#include <util/log.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace node {

namespace {

constexpr size_t PARSED_BYTES_PER_TX{256};
constexpr size_t PARSED_BYTES_PER_INPUT{96};
constexpr size_t PARSED_BYTES_PER_OUTPUT{64};

void CommitGroup(CommitJob& job, size_t g)
{
    using Item = BlockApplyView::CommitItem;
    const uint32_t count{static_cast<uint32_t>(job.blocks.size())};
    CoinsStore& store{job.blocks[0]->view->Store()};
    const size_t shards{store.ShardCount()};
    const size_t shard_begin{g * shards / COMMIT_GROUPS};
    const size_t shard_end{std::max(shard_begin + 1, (g + 1) * shards / COMMIT_GROUPS)};
    const size_t local_shards{shard_end - shard_begin};

    size_t total_items{0};
    for (uint32_t b{0}; b < count; ++b) {
        const StagedConnect& s{*job.blocks[b]};
        total_items += s.group_starts[g + 1] - s.group_starts[g];
    }
    if (total_items == 0) return;

    // Group items across all blocks b = 0..count-1 by shard (preserving block order within each shard)
    // so each shard mutex is acquired once for the entire CommitJob.
    struct Origin {
        uint32_t b;
        uint32_t i;
    };
    std::vector<uint32_t> counts(local_shards + 1, 0);
    std::vector<uint32_t> item_shard;
    item_shard.reserve(total_items);
    for (uint32_t b{0}; b < count; ++b) {
        const StagedConnect& s{*job.blocks[b]};
        for (uint32_t i{s.group_starts[g]}; i < s.group_starts[g + 1]; ++i) {
            const size_t s_idx{store.ShardIndexOf(s.items[i].Outpoint())};
            Assert(s_idx >= shard_begin && s_idx < shard_end);
            const uint32_t local{static_cast<uint32_t>(s_idx - shard_begin)};
            item_shard.push_back(local);
            ++counts[local + 1];
        }
    }
    for (size_t ls{0}; ls < local_shards; ++ls) counts[ls + 1] += counts[ls];

    std::vector<COutPoint> outpoints(total_items);
    std::vector<Coin> coins(total_items);
    std::vector<CoinsStore::Change> changes(total_items);
    std::vector<Origin> origins(total_items);
    std::vector<uint32_t> next(counts.begin(), counts.end() - 1);

    size_t flat{0};
    for (uint32_t b{0}; b < count; ++b) {
        const StagedConnect& s{*job.blocks[b]};
        const bool same_gen{s.resolve_flush_gen == job.store_flush_gen};
        const int height{job.heights[b]};
        for (uint32_t i{s.group_starts[g]}; i < s.group_starts[g + 1]; ++i) {
            const uint32_t slot{next[item_shard[flat++]]++};
            const Item& item{s.items[i]};
            outpoints[slot] = item.Outpoint();
            origins[slot] = {b, i};
            Coin& coin{coins[slot]};
            if (item.Spend()) {
                coin.nHeight = item.n >> 1;
                coin.fCoinBase = item.n & 1;
            } else {
                coin.out.nValue = item.Tx().vout[item.n].nValue;
                coin.nHeight = height;
                coin.fCoinBase = item.Tx().IsCoinBase();
            }
            changes[slot] = {
                .outpoint = &outpoints[slot],
                .coin = &coin,
                .possible_overwrite = (item.flags & Item::OVERWRITE) != 0,
                .known_coin = (item.flags & Item::KNOWN) != 0,
                .trust_from_base = same_gen && (item.flags & Item::FROM_BASE) != 0,
                .script = item.Spend() ? nullptr : &item.Tx().vout[item.n].scriptPubKey,
            };
        }
    }

    uint32_t earliest_conflict{CommitJob::NO_CONFLICT};
    CoinsStore::ConfirmedReport report;
    for (size_t ls{0}; ls < local_shards; ++ls) {
        const uint32_t begin{counts[ls]};
        const uint32_t end{counts[ls + 1]};
        if (begin == end) continue;
        report.conflicted.clear();
        report.overwritten.clear();
        report.unread.clear();
        const uint32_t conflicts{store.ApplyShard(
            shard_begin + ls,
            std::span<const CoinsStore::Change>{changes}.subspan(begin, end - begin),
            &report)};
        if (!report.unread.empty()) {
            LOCK(job.report_mutex);
            for (const COutPoint* at : report.unread) {
                const Origin& o{origins[at - outpoints.data()]};
                job.unread.emplace_back(o.b, *at);
            }
        }
        if (!report.overwritten.empty()) {
            LOCK(job.report_mutex);
            for (auto& [at, coin] : report.overwritten) {
                const Origin& o{origins[at - outpoints.data()]};
                job.overwritten.push_back({o.b, o.i, std::move(coin)});
            }
        }
        if (conflicts > 0) {
            for (const COutPoint* at : report.conflicted) {
                const Origin& o{origins[at - outpoints.data()]};
                job.blocks[o.b]->items[o.i].flags |= Item::CONFLICT;
                earliest_conflict = std::min(earliest_conflict, o.b);
            }
        }
    }
    if (earliest_conflict != CommitJob::NO_CONFLICT) {
        uint32_t first{job.first_conflict.load(std::memory_order_relaxed)};
        while (earliest_conflict < first && !job.first_conflict.compare_exchange_weak(first, earliest_conflict, std::memory_order_relaxed)) {}
    }
}

} // namespace

size_t UndoBytes(const CBlockUndo& undo)
{
    size_t bytes{memusage::DynamicUsage(undo.vtxundo)};
    for (const CTxUndo& tx : undo.vtxundo) {
        bytes += memusage::DynamicUsage(tx.vprevout);
        for (const Coin& coin : tx.vprevout) bytes += coin.DynamicMemoryUsage();
    }
    return bytes;
}

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

void PrepareCommit(StagedConnect& s, const CBlock& block, const CBlockUndo& undo, std::span<const uint8_t> from_base)
{
    if (!s.pindex->pprev) {
        s.group_starts.assign(COMMIT_GROUPS + 1, 0);
        s.view->ChangesCommitted();
        return;
    }
    using Item = BlockApplyView::CommitItem;
    const uint32_t own_height{static_cast<uint32_t>(s.pindex->nHeight)};
    const CoinsStore& store{s.view->Store()};
    const size_t shards{store.ShardCount()};
    static_assert(COMMIT_GROUPS < 0xFF);
    static constexpr uint8_t SKIP{0xFF};

    static thread_local std::vector<uint8_t> groups;
    groups.clear();
    s.group_starts.assign(COMMIT_GROUPS + 1, 0);
    const auto group_of{[&](const COutPoint& outpoint) {
        const uint8_t g{static_cast<uint8_t>(store.ShardIndexOf(outpoint) * COMMIT_GROUPS / shards)};
        ++s.group_starts[g + 1];
        return g;
    }};

    std::unordered_set<COutPoint, SaltedCoinsCacheHasher> own_spent;
    Assert(undo.vtxundo.size() + 1 == block.vtx.size());
    for (size_t t{1}; t < block.vtx.size(); ++t) {
        const CTransaction& tx{*block.vtx[t]};
        const CTxUndo& tx_undo{undo.vtxundo[t - 1]};
        Assert(tx_undo.vprevout.size() == tx.vin.size());
        for (size_t j{0}; j < tx.vin.size(); ++j) {
            if (tx_undo.vprevout[j].nHeight == own_height) {
                own_spent.insert(tx.vin[j].prevout);
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
            const bool spent_here{!own_spent.empty() && own_spent.contains(outpoint)};
            groups.push_back(spent_here ? SKIP : group_of(outpoint));
        }
    }

    for (size_t g{0}; g < COMMIT_GROUPS; ++g) s.group_starts[g + 1] += s.group_starts[g];
    std::array<uint32_t, COMMIT_GROUPS> next;
    std::copy(s.group_starts.begin(), s.group_starts.end() - 1, next.begin());
    s.items.resize(s.group_starts[COMMIT_GROUPS]);
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
            const uint8_t flags{static_cast<uint8_t>(Item::SPEND | Item::KNOWN | (base_read ? Item::FROM_BASE : 0))};
            s.items[next[groups[k++]]++] = {
                .at = &tx.vin[j].prevout,
                .n = (uint32_t{coin.nHeight} << 1) | uint32_t{coin.fCoinBase},
                .flags = flags,
            };
        }
    }
    for (size_t t{0}; t < block.vtx.size(); ++t) {
        const CTransaction& tx{*block.vtx[t]};
        const uint8_t flags{t == 0 ? Item::OVERWRITE : Item::FRESH};
        for (uint32_t n{0}; n < tx.vout.size(); ++n) {
            if (tx.vout[n].scriptPubKey.IsUnspendable()) continue;
            const uint8_t g{groups[k++]};
            if (g == SKIP) continue;
            s.items[next[g]++] = {.at = &tx, .n = n, .flags = flags};
        }
    }
    Assert(k == groups.size());
    s.view->ChangesCommitted();
}

std::shared_ptr<CommitJob> StartCommitJob(WindowSync& sync, StagedConnect& front, size_t max_blocks, int max_height)
{
    AssertLockHeld(sync.mutex);
    Assert(!sync.active_job && !front.committed && !front.in_job);
    auto job{std::make_shared<CommitJob>()};
    job->store_flush_gen = front.view->Store().FlushGeneration();
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

    if (job->groups_done.load(std::memory_order_acquire) < COMMIT_GROUPS) {
        for (size_t g{0}; g < COMMIT_GROUPS; ++g) {
            if (job->group_state[g].load(std::memory_order_relaxed) != 0) continue;
            uint8_t expected{0};
            if (!job->group_state[g].compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) continue;
            try {
                CommitGroup(*job, g);
            } catch (const std::exception&) {
                LOCK(job->report_mutex);
                if (!job->error) job->error = std::current_exception();
            }
            job->group_state[g].store(2, std::memory_order_release);
            job->groups_done.fetch_add(1, std::memory_order_acq_rel);
        }
    }

    if (job->groups_done.load(std::memory_order_acquire) < COMMIT_GROUPS) return;
    if (job->finalizing.exchange(true, std::memory_order_acq_rel)) return;

    sync.job_open.store(false, std::memory_order_release);
    const bool has_error{WITH_LOCK(job->report_mutex, return job->error != nullptr)};

    {
        LOCK(sync.mutex);
        job->ready = true;
        if (sync.active_job == job) {
            sync.active_job.reset();
            const uint32_t first_conflict{has_error ? 0 : job->first_conflict.load(std::memory_order_relaxed)};
            for (uint32_t b{0}; b < job->blocks.size(); ++b) {
                StagedConnect* const s{job->blocks[b]};
                if (b >= first_conflict) {
                    s->commit_failed = true;
                } else {
                    s->committed = true;
                }
            }
        }
    }
    sync.cv.notify_all();
}

bool TakeBackCommit(CommitJob& job,
                    uint32_t first_to_revert,
                    const std::function<bool(const CBlockIndex&, CBlockUndo&)>& read_undo)
{
    using Item = BlockApplyView::CommitItem;
    const uint32_t count{static_cast<uint32_t>(job.blocks.size())};
    Assert(first_to_revert < count);
    CoinsStore& store{job.blocks.front()->view->Store()};
    const std::vector<CommitJob::Overwritten> overwritten{WITH_LOCK(job.report_mutex, return std::move(job.overwritten))};
    const std::vector<std::pair<uint32_t, COutPoint>> unread{WITH_LOCK(job.report_mutex, return std::move(job.unread))};

    for (uint32_t b{count}; b-- > first_to_revert;) {
        StagedConnect& s{*job.blocks[b]};
        const CBlock& block{*Assert(s.block)};
        CBlockUndo read;
        const CBlockUndo* undo{s.undo.get()};
        if (!s.write_undo || !undo) {
            if (!read_undo(*s.pindex, read)) return false;
            undo = &read;
        }
        Assert(undo->vtxundo.size() + 1 == block.vtx.size());
        std::unordered_map<const COutPoint*, const Coin*> spent;
        for (size_t t{1}; t < block.vtx.size(); ++t) {
            const CTransaction& tx{*block.vtx[t]};
            const CTxUndo& tx_undo{undo->vtxundo[t - 1]};
            Assert(tx_undo.vprevout.size() == tx.vin.size());
            for (size_t j{0}; j < tx.vin.size(); ++j) spent.emplace(&tx.vin[j].prevout, &tx_undo.vprevout[j]);
        }

        std::vector<COutPoint> outpoints;
        std::vector<Coin> coins;
        std::vector<CoinsStore::Change> part;
        outpoints.reserve(s.items.size());
        coins.reserve(s.items.size());
        part.reserve(s.items.size());
        for (uint32_t i{0}; i < s.items.size(); ++i) {
            const Item& item{s.items[i]};
            if (item.flags & Item::CONFLICT) continue;
            outpoints.push_back(item.Outpoint());
            Coin& coin{coins.emplace_back()};
            if (item.Spend()) {
                coin = *spent.at(static_cast<const COutPoint*>(item.at));
                coin.nHeight = item.n >> 1;
                coin.fCoinBase = item.n & 1;
            } else {
                bool found{false};
                for (const CommitJob::Overwritten& over : overwritten) {
                    if (over.b == b && over.i == i) {
                        coin = over.coin;
                        found = true;
                    }
                }
                if (!found && std::ranges::any_of(unread, [&](const auto& u) { return u.first == b && u.second == outpoints.back(); })) {
                    if (std::optional<Coin> base{store.BaseCoin(outpoints.back())}) coin = std::move(*base);
                }
            }
            part.push_back({.outpoint = &outpoints.back(), .coin = &coin, .possible_overwrite = true});
        }
        Assert(store.ApplyConfirmed(std::move(part)) == 0);
    }

    LogInfo("Commit conflict at height %d: reverted %u blocks from CoinsStore for serial retry",
            job.heights[first_to_revert], count - first_to_revert);
    for (uint32_t b{first_to_revert}; b < count; ++b) {
        std::vector<BlockApplyView::CommitItem>{}.swap(job.blocks[b]->items);
    }
    return true;
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
        staged.view->Release();
    }
    in_flight.clear();
}

} // namespace node
