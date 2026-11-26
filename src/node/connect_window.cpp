// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/connect_window.h>

#include <logging.h>
#include <memusage.h>
#include <util/check.h>
#include <util/hasher.h>

#include <algorithm>
#include <bit>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace node {

namespace {

constexpr size_t PARSED_BYTES_PER_TX{sizeof(CTransaction) + 64};
constexpr size_t PARSED_BYTES_PER_INPUT{sizeof(CTxIn) + 80};
constexpr size_t PARSED_BYTES_PER_OUTPUT{sizeof(CTxOut)};

void CommitGroup(CommitJob& job, size_t g)
{
    using Item = BlockApplyView::CommitItem;
    const uint32_t count{static_cast<uint32_t>(job.blocks.size())};
    if (count > 1) {
        // Net outputs created and spent within this same CommitJob using a process-salted hash map.
        struct CreatedAt {
            uint32_t b;
            uint32_t i;
            bool dead{false};
        };
        std::unordered_map<COutPoint, CreatedAt, SaltedCoinsCacheHasher> created;
        for (uint32_t b{0}; b < count; ++b) {
            StagedConnect& s{*job.blocks[b]};
            for (uint32_t i{s.group_starts[g]}; i < s.group_starts[g + 1]; ++i) {
                const Item& item{s.items[i]};
                if (!item.Spend()) {
                    const bool fresh{(item.flags & Item::FRESH) != 0};
                    auto [it, inserted]{created.try_emplace(item.Outpoint(), CreatedAt{b, i, !fresh})};
                    if (!inserted) it->second.dead = true;
                } else if (item.flags & Item::KNOWN) {
                    const auto it{created.find(item.Outpoint())};
                    if (it == created.end() || it->second.dead) continue;
                    const uint32_t cb{it->second.b};
                    const uint32_t ci{it->second.i};
                    if (job.heights[cb] != static_cast<int>(item.n >> 1)) continue;
                    job.blocks[cb]->items[ci].flags |= Item::DROPPED;
                    s.items[i].flags |= Item::DROPPED;
                    it->second.dead = true;
                }
            }
        }
    }

    std::vector<COutPoint> outpoints;
    std::vector<Coin> coins;
    for (uint32_t b{0}; b < count; ++b) {
        StagedConnect& s{*job.blocks[b]};
        const size_t size{s.group_starts[g + 1] - s.group_starts[g]};
        outpoints.clear();
        coins.clear();
        outpoints.reserve(size);
        coins.reserve(size);
        std::vector<CoinsStore::Change> part;
        part.reserve(size);
        for (uint32_t i{s.group_starts[g]}; i < s.group_starts[g + 1]; ++i) {
            const Item& item{s.items[i]};
            if (item.flags & Item::DROPPED) continue;
            outpoints.push_back(item.Outpoint());
            if (item.Spend()) {
                Coin& coin{coins.emplace_back()};
                coin.nHeight = item.n >> 1;
                coin.fCoinBase = item.n & 1;
            } else {
                Coin& coin{coins.emplace_back()};
                coin.out.nValue = item.Tx().vout[item.n].nValue;
                coin.nHeight = job.heights[b];
                coin.fCoinBase = item.Tx().IsCoinBase();
            }
            part.push_back({
                .outpoint = &outpoints.back(),
                .coin = &coins.back(),
                .possible_overwrite = (item.flags & Item::OVERWRITE) != 0,
                .known_coin = (item.flags & Item::KNOWN) != 0,
                .script = item.Spend() ? nullptr : &item.Tx().vout[item.n].scriptPubKey,
            });
        }
        if (part.empty()) continue;
        CoinsStore::ConfirmedReport report;
        const uint32_t conflicts{s.view->Store().ApplyConfirmed(std::move(part), &report)};
        if (!report.unread.empty()) {
            LOCK(job.report_mutex);
            for (const COutPoint* at : report.unread) job.unread.emplace_back(b, *at);
        }
        if (conflicts == 0 && report.overwritten.empty()) continue;

        std::vector<uint32_t> item_of;
        for (uint32_t i{s.group_starts[g]}; i < s.group_starts[g + 1]; ++i) {
            if (!(s.items[i].flags & Item::DROPPED)) item_of.push_back(i);
        }
        for (const COutPoint* at : report.conflicted) {
            s.items[item_of[at - outpoints.data()]].flags |= Item::CONFLICT;
        }
        if (!report.overwritten.empty()) {
            LOCK(job.report_mutex);
            for (auto& [at, coin] : report.overwritten) {
                job.overwritten.push_back({b, item_of[at - outpoints.data()], std::move(coin)});
            }
        }
        if (conflicts == 0) continue;
        uint32_t first{job.first_conflict.load(std::memory_order_relaxed)};
        while (b < first && !job.first_conflict.compare_exchange_weak(first, b, std::memory_order_relaxed)) {}
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

void PrepareCommit(StagedConnect& s, const CBlock& block, const CBlockUndo& undo)
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
    for (size_t t{1}; t < block.vtx.size(); ++t) {
        const CTransaction& tx{*block.vtx[t]};
        const CTxUndo& tx_undo{undo.vtxundo[t - 1]};
        for (size_t j{0}; j < tx.vin.size(); ++j) {
            const Coin& coin{tx_undo.vprevout[j]};
            if (coin.nHeight == own_height) continue;
            s.items[next[groups[k++]]++] = {
                .at = &tx.vin[j].prevout,
                .n = (uint32_t{coin.nHeight} << 1) | uint32_t{coin.fCoinBase},
                .flags = static_cast<uint8_t>(Item::SPEND | Item::KNOWN),
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

    const size_t count{job->blocks.size()};
    if (job->groups_done.load(std::memory_order_acquire) < COMMIT_GROUPS) {
        for (size_t g{0}; g < COMMIT_GROUPS; ++g) {
            if (job->group_state[g].load(std::memory_order_relaxed) != 0) continue;
            uint8_t expected{0};
            if (!job->group_state[g].compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) continue;
            try {
                CommitGroup(*job, g);
            } catch (const std::runtime_error&) {
                LOCK(job->report_mutex);
                if (!job->error) job->error = std::current_exception();
            }
            job->group_state[g].store(2, std::memory_order_release);
            job->groups_done.fetch_add(1, std::memory_order_acq_rel);
        }
    }

    if (job->groups_done.load(std::memory_order_acquire) < COMMIT_GROUPS) return;

    const bool has_error{WITH_LOCK(job->report_mutex, return job->error != nullptr)};
    const bool has_conflict{job->first_conflict.load(std::memory_order_relaxed) != CommitJob::NO_CONFLICT};

    bool finished{false};
    for (size_t b; (b = job->next_clean.fetch_add(1, std::memory_order_relaxed)) < count;) {
        if (b + 1 == count) sync.job_open.store(false, std::memory_order_release);
        StagedConnect& s{*job->blocks[b]};
        if (!has_conflict && !has_error) {
            std::vector<BlockApplyView::CommitItem>{}.swap(s.items);
        }
        if (job->cleaned.fetch_add(1, std::memory_order_acq_rel) + 1 == count) finished = true;
    }
    if (!finished) return;

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
            if (item.flags & (Item::DROPPED | Item::CONFLICT)) continue;
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

    for (uint32_t b{first_to_revert}; b < count; ++b) {
        const StagedConnect& s{*job.blocks[b]};
        for (size_t g{0}; g < COMMIT_GROUPS; ++g) {
            for (uint32_t i{s.group_starts[g]}; i < s.group_starts[g + 1]; ++i) {
                const Item& item{s.items[i]};
                if (!item.Spend() || !(item.flags & Item::DROPPED)) continue;
                const int made{static_cast<int>(item.n >> 1)};
                const auto at{std::find(job.heights.begin(), job.heights.begin() + first_to_revert, made)};
                if (at == job.heights.begin() + first_to_revert) continue;
                const StagedConnect& maker{*job.blocks[at - job.heights.begin()]};
                const COutPoint outpoint{item.Outpoint()};
                for (uint32_t c{maker.group_starts[g]}; c < maker.group_starts[g + 1]; ++c) {
                    const Item& create{maker.items[c]};
                    if (create.Spend() || !(create.flags & Item::DROPPED) || create.Outpoint() != outpoint) continue;
                    Coin coin;
                    coin.out.nValue = create.Tx().vout[create.n].nValue;
                    coin.nHeight = made;
                    coin.fCoinBase = create.Tx().IsCoinBase();
                    std::vector<CoinsStore::Change> one;
                    one.push_back({
                        .outpoint = &outpoint,
                        .coin = &coin,
                        .possible_overwrite = (create.flags & Item::OVERWRITE) != 0,
                        .script = &create.Tx().vout[create.n].scriptPubKey,
                    });
                    Assert(store.ApplyConfirmed(std::move(one)) == 0);
                    break;
                }
            }
        }
    }

    LogInfo("Commit conflict at height %d: reverted %u blocks from CoinsStore for serial retry",
            job.heights[first_to_revert], count - first_to_revert);
    for (StagedConnect* s : job.blocks) std::vector<BlockApplyView::CommitItem>{}.swap(s->items);
    return true;
}

void DrainWindow(std::shared_ptr<WindowSync>& sync,
                 std::deque<std::unique_ptr<StagedConnect>>& in_flight,
                 util::ByteAccount& window_account)
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
    window_account.Set(0);
}

} // namespace node
