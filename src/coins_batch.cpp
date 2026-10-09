// Copyright (c) 2026 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#include <coins_batch.h>

#include <memusage.h>
#include <primitives/block.h>
#include <util/check.h>
#include <util/threadpool.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <exception>
#include <functional>
#include <future>
#include <limits>
#include <tuple>
#include <utility>
#include <vector>

/** Owned by a batch, referenced only while the confirmed cache is pinned. */
struct CoinsCacheProvisionalRecord {
    COutPoint outpoint;
    CoinsCachePair* entry{nullptr};
    const Coin* source{nullptr};
    Coin* creation{nullptr};
    bool inserted{false};
    bool base_unspent{false};
    bool created{false};
    bool spent{false};
    bool coinbase{false};
};

namespace {

size_t AddBound(size_t left, size_t right)
{
    if (right > std::numeric_limits<size_t>::max() - left) {
        return std::numeric_limits<size_t>::max();
    }
    return left + right;
}

size_t MultiplyBound(size_t count, size_t bytes)
{
    if (bytes && count > std::numeric_limits<size_t>::max() / bytes) {
        return std::numeric_limits<size_t>::max();
    }
    return count * bytes;
}

} // namespace

struct CoinsViewBatch::Impl {
    enum class EventType {
        PROBE,
        READ,
        SPEND,
        CREATE
    };
    struct Event {
        COutPoint outpoint;
        size_t block;
        size_t tx;
        size_t index;
        size_t creation;
        EventType type;
    };
    struct BlockData {
        BlockDescriptor descriptor;
        std::vector<std::vector<const Coin*>> inputs;
        CBlockUndo undo;
        std::vector<Coin> creations;
    };
    struct Partition {
        std::vector<Event> events;
        std::vector<CoinsCacheProvisionalRecord> records;
        size_t initial_map_usage{0};
        size_t charged_map_usage{0};
    };

    CCoinsViewCache& cache;
    size_t limit;
    size_t initial_cache_usage;
    std::vector<BlockData> blocks;
    std::array<Partition, COINS_CACHE_PARTITIONS> partitions;
    std::atomic<size_t> allocated{0};
    std::atomic<bool> fallback{false};
    bool prepared{false};
    bool active{true};
    bool admitted{false};

    Impl(CCoinsViewCache& cache_in, size_t limit_in)
        : cache{cache_in}, limit{limit_in}, initial_cache_usage{cache.DynamicMemoryUsage()} {}

    static size_t Estimate(const CCoinsViewCache& cache, std::span<const BlockDescriptor> blocks, std::array<size_t, COINS_CACHE_PARTITIONS>& event_counts);

    // Range submission accepts all partitions or none. Rejection completes
    // inline; promotion cannot be cancelled. Allocation failure is fail-stop.
    template <typename F>
    void RunPartitions(ThreadPool& pool, F&& job) noexcept
    {
        std::array<std::function<void()>, COINS_CACHE_PARTITIONS> tasks;
        for (size_t i{0}; i < COINS_CACHE_PARTITIONS; ++i) {
            tasks[i] = [&job, i]() noexcept { job(i); };
        }
        if (auto futures{pool.Submit(std::move(tasks))}) {
            for (auto& future : *futures) {
                future.get();
            }
        } else {
            for (size_t i{0}; i < COINS_CACHE_PARTITIONS; ++i) {
                job(i);
            }
        }
    }

    void PromotePartition(size_t partition_index) noexcept
    {
        auto& storage{cache.m_storage[partition_index]};
        for (auto& record : partitions[partition_index].records) {
            auto& pair{*record.entry};
            auto& entry{pair.second};
            if (!record.created && !record.spent) {
                if (record.inserted) {
                    storage.coins_usage -= entry.coin.DynamicMemoryUsage();
                    storage.coins.erase(record.outpoint);
                }
                continue;
            }
            if (!record.source) {
                const bool fresh_creation{record.created && !record.coinbase && !entry.IsDirty()};
                if (entry.IsFresh() || fresh_creation) {
                    storage.coins_usage -= entry.coin.DynamicMemoryUsage();
                    storage.dirty_count -= entry.IsDirty();
                    storage.coins.erase(record.outpoint);
                    continue;
                }
                if (record.created && !record.coinbase && entry.IsDirty() && entry.coin.IsSpent()) {
                    continue;
                }
                storage.coins_usage -= entry.coin.DynamicMemoryUsage();
                entry.coin.Clear();
            } else {
                Assert(record.created);
                const bool fresh{!record.coinbase && !entry.IsDirty()};
                storage.coins_usage -= entry.coin.DynamicMemoryUsage();
                entry.coin = std::move(*record.creation);
                storage.coins_usage += entry.coin.DynamicMemoryUsage();
                if (fresh) {
                    CCoinsCacheEntry::SetFresh(pair, storage.sentinel);
                }
            }
            entry.SetConfirmedUnknown(false);
            if (!entry.IsDirty()) {
                CCoinsCacheEntry::SetDirty(pair, storage.sentinel);
                ++storage.dirty_count;
            }
        }
    }

    void CleanPartition(size_t partition_index) noexcept
    {
        auto& partition{partitions[partition_index]};
        std::vector<Event>{}.swap(partition.events);
        std::vector<CoinsCacheProvisionalRecord>{}.swap(partition.records);
        // Promotion readers have joined; each job destroys distinct elements.
        // The owner keeps the containing vector unchanged until cleanup joins.
        for (size_t i{partition_index}; i < blocks.size(); i += COINS_CACHE_PARTITIONS) {
            BlockData empty{};
            std::swap(empty, blocks[i]);
        }
    }

    void Release() noexcept
    {
        std::vector<BlockData>{}.swap(blocks);
        for (auto& partition : partitions) {
            std::vector<Event>{}.swap(partition.events);
            std::vector<CoinsCacheProvisionalRecord>{}.swap(partition.records);
        }
    }

    bool Charge(size_t bytes)
    {
        if (bytes == 0) {
            return true;
        }
        if (bytes > limit) {
            fallback.store(true);
            return false;
        }
        const auto previous{allocated.fetch_add(bytes)};
        if (previous > limit - bytes) {
            fallback.store(true);
            return false;
        }
        return true;
    }

    size_t GroupUsage() const
    {
        size_t usage{memusage::MallocUsage(sizeof(Impl)) + memusage::DynamicUsage(blocks)};
        for (const auto& block : blocks) {
            usage += memusage::DynamicUsage(block.inputs) + memusage::DynamicUsage(block.undo.vtxundo) + memusage::DynamicUsage(block.creations);
            for (const auto& inputs : block.inputs) {
                usage += memusage::DynamicUsage(inputs);
            }
            for (const auto& undo : block.undo.vtxundo) {
                usage += memusage::DynamicUsage(undo.vprevout);
                for (const auto& coin : undo.vprevout) {
                    usage += coin.DynamicMemoryUsage();
                }
            }
            for (const auto& coin : block.creations) {
                usage += coin.DynamicMemoryUsage();
            }
        }
        for (const auto& partition : partitions) {
            usage += memusage::DynamicUsage(partition.events) + memusage::DynamicUsage(partition.records);
        }
        return usage;
    }

    bool LoadBase(CoinsCacheProvisionalRecord& record, CoinsCacheStorage& storage)
    {
        auto& entry{record.entry->second};
        if (!entry.IsConfirmedUnknown()) {
            record.source = entry.coin.IsSpent() ? nullptr : &entry.coin;
            record.base_unspent = record.source != nullptr;
            return true;
        }
        // PeekCoin never mutates a parent cache. Its backend supports concurrent reads.
        auto coin{cache.base->PeekCoin(record.outpoint)};
        if (coin && !Charge(coin->DynamicMemoryUsage())) {
            return false;
        }
        entry.SetConfirmedUnknown(false);
        if (coin) {
            storage.coins_usage += coin->DynamicMemoryUsage();
            entry.coin = std::move(*coin);
            record.source = &entry.coin;
            record.base_unspent = true;
        }
        return true;
    }

    void Resolve(size_t partition_index)
    {
        auto& partition{partitions[partition_index]};
        auto& storage{cache.m_storage[partition_index]};
        const auto ordering = [](const Event& event) {
            // Every block-entry BIP30 probe precedes all transaction events.
            return std::tuple{event.block, event.type != EventType::PROBE, event.tx, event.type, event.index};
        };
        std::sort(partition.events.begin(), partition.events.end(), [&](const Event& left, const Event& right) {
            if (left.outpoint != right.outpoint) {
                return left.outpoint < right.outpoint;
            }
            return ordering(left) < ordering(right);
        });
        for (size_t begin{0}; begin < partition.events.size();) {
            if (fallback.load()) {
                return;
            }
            size_t end{begin + 1};
            while (end < partition.events.size() && partition.events[begin].outpoint == partition.events[end].outpoint) {
                ++end;
            }
            partition.records.emplace_back();
            auto& record{partition.records.back()};
            record.outpoint = partition.events[begin].outpoint;
            auto [it, inserted]{storage.coins.try_emplace(record.outpoint)};
            record.entry = &*it;
            record.inserted = inserted;
            auto& entry{it->second};
            if (inserted) {
                entry.SetConfirmedUnknown(true);
            }
            const auto map_usage{memusage::DynamicUsage(storage.coins)};
            const auto growth{map_usage > partition.initial_map_usage ? map_usage - partition.initial_map_usage : 0};
            if (growth > partition.charged_map_usage) {
                const auto delta{growth - partition.charged_map_usage};
                partition.charged_map_usage = growth;
                if (!Charge(delta)) {
                    return;
                }
            }
            record.source = entry.coin.IsSpent() ? nullptr : &entry.coin;
            record.base_unspent = record.source != nullptr;
            for (size_t event_index{begin}; event_index < end; ++event_index) {
                const auto& event{partition.events[event_index]};
                switch (event.type) {
                case EventType::PROBE:
                    if (!record.created && !record.spent && !LoadBase(record, storage)) {
                        return;
                    }
                    if (record.source) {
                        fallback.store(true);
                        return;
                    }
                    break;
                case EventType::READ: {
                    if (!record.created && !record.spent && !LoadBase(record, storage)) {
                        return;
                    }
                    if (!record.source) {
                        fallback.store(true);
                        return;
                    }
                    auto& block{blocks[event.block]};
                    block.inputs[event.tx][event.index] = record.source;
                    if (!Charge(record.source->DynamicMemoryUsage())) {
                        return;
                    }
                    block.undo.vtxundo[event.tx - 1].vprevout[event.index] = *record.source;
                    break;
                }
                case EventType::SPEND:
                    if (!record.source) {
                        fallback.store(true);
                        return;
                    }
                    record.source = nullptr;
                    record.spent = true;
                    break;
                case EventType::CREATE:
                    if (record.created || record.base_unspent) {
                        fallback.store(true);
                        return;
                    }
                    record.created = true;
                    record.spent = false;
                    record.creation = &blocks[event.block].creations[event.creation];
                    record.source = record.creation;
                    record.coinbase = record.creation->IsCoinBase();
                    break;
                }
            }
            begin = end;
        }
    }
};

size_t CoinsViewBatch::Impl::Estimate(const CCoinsViewCache& cache, std::span<const BlockDescriptor> blocks, std::array<size_t, COINS_CACHE_PARTITIONS>& event_counts)
{
    size_t estimate{memusage::MallocUsage(sizeof(Impl))};
    estimate = AddBound(estimate, MultiplyBound(blocks.size(), sizeof(Impl::BlockData) + 64));
    size_t events{0};
    for (const auto& descriptor : blocks) {
        if (!descriptor.block || descriptor.height < 1 || descriptor.block->vtx.empty() || !descriptor.block->vtx.front()->IsCoinBase()) {
            return std::numeric_limits<size_t>::max();
        }
        estimate = AddBound(estimate, MultiplyBound(descriptor.block->vtx.size(), sizeof(std::vector<const Coin*>) + sizeof(CTxUndo) + 64));
        for (const auto& tx : descriptor.block->vtx) {
            if (!tx->IsCoinBase()) {
                for (const auto& input : tx->vin) {
                    auto& count{event_counts[cache.m_partition_hasher(input.prevout) % COINS_CACHE_PARTITIONS]};
                    count = AddBound(count, 2);
                }
                events = AddBound(events, MultiplyBound(tx->vin.size(), 2));
                estimate = AddBound(estimate, MultiplyBound(tx->vin.size(), sizeof(const Coin*) + sizeof(Coin) + 64));
            }
            events = AddBound(events, MultiplyBound(tx->vout.size(), descriptor.enforce_bip30 ? 2 : 1));
            estimate = AddBound(estimate, MultiplyBound(tx->vout.size(), sizeof(Coin) + 64));
            for (size_t i{0}; i < tx->vout.size(); ++i) {
                const auto& out{tx->vout[i]};
                const COutPoint outpoint{tx->GetHash(), static_cast<uint32_t>(i)};
                auto& count{event_counts[cache.m_partition_hasher(outpoint) % COINS_CACHE_PARTITIONS]};
                count = AddBound(count, descriptor.enforce_bip30 + !out.scriptPubKey.IsUnspendable());
                if (!out.scriptPubKey.IsUnspendable()) {
                    estimate = AddBound(estimate, memusage::DynamicUsage(out.scriptPubKey));
                }
            }
        }
    }
    // Allow map-node/pool growth and a possible larger bucket array in each shard.
    estimate = AddBound(estimate, MultiplyBound(events, sizeof(Impl::Event) + sizeof(CoinsCacheProvisionalRecord) + sizeof(CoinsCachePair) + 128));
    for (size_t i{0}; i < COINS_CACHE_PARTITIONS; ++i) {
        const auto& coins{cache.m_storage[i].coins};
        const auto max_size{AddBound(coins.size(), event_counts[i])};
        if (static_cast<long double>(max_size) > static_cast<long double>(coins.bucket_count()) * coins.max_load_factor()) {
            const auto bucket_bound{2 * static_cast<long double>(max_size) / coins.max_load_factor() + 64};
            if (bucket_bound > std::numeric_limits<size_t>::max() / sizeof(void*)) {
                return std::numeric_limits<size_t>::max();
            }
            estimate = AddBound(estimate, static_cast<size_t>(bucket_bound) * sizeof(void*));
        }
    }
    return AddBound(estimate, COINS_CACHE_PARTITIONS * 16384);
}

size_t CoinsViewBatch::EstimateScratch(const CCoinsViewCache& cache, std::span<const BlockDescriptor> blocks)
{
    std::array<size_t, COINS_CACHE_PARTITIONS> event_counts{};
    return Impl::Estimate(cache, blocks, event_counts);
}

CoinsViewBatch::CoinsViewBatch(CCoinsViewCache& cache, std::span<const BlockDescriptor> blocks, size_t scratch_limit)
    : m_impl{std::make_unique<Impl>(cache, scratch_limit)}
{
    Assert(!cache.m_active_batch);
    std::array<size_t, COINS_CACHE_PARTITIONS> event_counts{};
    if (Impl::Estimate(cache, blocks, event_counts) <= scratch_limit) {
        for (size_t i{0}; i < COINS_CACHE_PARTITIONS; ++i) {
            auto& partition{m_impl->partitions[i]};
            partition.events.reserve(event_counts[i]);
            partition.records.reserve(event_counts[i]);
            partition.initial_map_usage = memusage::DynamicUsage(cache.m_storage[i].coins);
        }
        m_impl->blocks.resize(blocks.size());
        for (size_t block_index{0}; block_index < blocks.size(); ++block_index) {
            auto& data{m_impl->blocks[block_index]};
            data.descriptor = blocks[block_index];
            const auto& block{*data.descriptor.block};
            data.inputs.resize(block.vtx.size());
            data.undo.vtxundo.resize(block.vtx.size() - 1);
            size_t output_count{0};
            for (const auto& tx : block.vtx) {
                output_count += tx->vout.size();
            }
            data.creations.resize(output_count);
            size_t creation{0};
            const auto add_event = [&](const COutPoint& outpoint, size_t tx, size_t index, size_t source, Impl::EventType type) {
                auto& partition{m_impl->partitions[cache.m_partition_hasher(outpoint) % COINS_CACHE_PARTITIONS]};
                partition.events.emplace_back(outpoint, block_index, tx, index, source, type);
            };
            for (size_t tx_index{0}; tx_index < block.vtx.size(); ++tx_index) {
                const auto& tx{*block.vtx[tx_index]};
                if (!tx.IsCoinBase()) {
                    data.inputs[tx_index].resize(tx.vin.size());
                    data.undo.vtxundo[tx_index - 1].vprevout.resize(tx.vin.size());
                    for (size_t input{0}; input < tx.vin.size(); ++input) {
                        add_event(tx.vin[input].prevout, tx_index, input, 0, Impl::EventType::READ);
                        add_event(tx.vin[input].prevout, tx_index, input, 0, Impl::EventType::SPEND);
                    }
                }
                for (size_t output{0}; output < tx.vout.size(); ++output, ++creation) {
                    const COutPoint outpoint{tx.GetHash(), static_cast<uint32_t>(output)};
                    if (data.descriptor.enforce_bip30) {
                        add_event(outpoint, tx_index, output, 0, Impl::EventType::PROBE);
                    }
                    if (!tx.vout[output].scriptPubKey.IsUnspendable()) {
                        data.creations[creation] = Coin{tx.vout[output], data.descriptor.height, tx.IsCoinBase()};
                        add_event(outpoint, tx_index, output, creation, Impl::EventType::CREATE);
                    }
                }
            }
        }
        m_impl->admitted = m_impl->Charge(m_impl->GroupUsage());
    }
    cache.m_active_batch = this;
}

CoinsViewBatch::~CoinsViewBatch()
{
    Cancel();
}

CoinsViewBatch::PrepareResult CoinsViewBatch::Prepare(ThreadPool& pool)
{
    Assert(m_impl->active && !m_impl->prepared);
    if (!m_impl->admitted) {
        Cancel();
        return PrepareResult::FALLBACK;
    }
    std::vector<std::future<void>> futures;
    futures.reserve(COINS_CACHE_PARTITIONS);
    bool interrupted{false};
    std::exception_ptr exception;
    try {
        for (size_t i{0}; i < COINS_CACHE_PARTITIONS; ++i) {
            auto result{pool.Submit([this, i] { m_impl->Resolve(i); })};
            if (result) {
                futures.emplace_back(std::move(*result));
            } else if (result.error() == ThreadPool::SubmitError::Inactive && futures.empty()) {
                for (size_t j{0}; j < COINS_CACHE_PARTITIONS; ++j) {
                    m_impl->Resolve(j);
                }
                break;
            } else {
                interrupted = true;
                break;
            }
        }
    } catch (...) {
        exception = std::current_exception();
    }
    // No error can release pins or source records before every submitted job joins.
    for (auto& future : futures) {
        try {
            future.get();
        } catch (...) {
            if (!exception) {
                exception = std::current_exception();
            }
        }
    }
    if (exception || interrupted || m_impl->fallback.load() || DynamicMemoryUsage() > m_impl->limit) {
        Cancel();
        if (exception) {
            std::rethrow_exception(exception);
        }
        return interrupted ? PrepareResult::INTERRUPTED : PrepareResult::FALLBACK;
    }
    m_impl->prepared = true;
    return PrepareResult::READY;
}

std::span<const Coin* const> CoinsViewBatch::Inputs(size_t block, size_t tx) const
{
    Assert(m_impl->active && m_impl->prepared);
    return m_impl->blocks.at(block).inputs.at(tx);
}

const CBlockUndo& CoinsViewBatch::Undo(size_t block) const
{
    Assert(m_impl->active && m_impl->prepared);
    return m_impl->blocks.at(block).undo;
}

size_t CoinsViewBatch::DynamicMemoryUsage() const
{
    const auto cache_usage{m_impl->cache.DynamicMemoryUsage()};
    return m_impl->GroupUsage() + (cache_usage > m_impl->initial_cache_usage ? cache_usage - m_impl->initial_cache_usage : 0);
}

CoinsViewBatch::CommitTimings CoinsViewBatch::Commit(const uint256& last_hash, ThreadPool& pool) noexcept
{
    Assert(m_impl->active && m_impl->prepared);
    const auto start{std::chrono::steady_clock::now()};
    m_impl->RunPartitions(pool, [this](size_t i) noexcept { m_impl->PromotePartition(i); });
    const auto promoted{std::chrono::steady_clock::now()};
    m_impl->RunPartitions(pool, [this](size_t i) noexcept { m_impl->CleanPartition(i); });
    std::vector<Impl::BlockData>{}.swap(m_impl->blocks);
    const auto cleaned{std::chrono::steady_clock::now()};
    m_impl->cache.m_block_hash = last_hash;
    m_impl->cache.m_active_batch = nullptr;
    m_impl->active = false;
    return {std::chrono::duration_cast<std::chrono::microseconds>(promoted - start),
            std::chrono::duration_cast<std::chrono::microseconds>(cleaned - promoted)};
}

void CoinsViewBatch::Cancel() noexcept
{
    if (!m_impl->active) {
        return;
    }
    for (size_t partition_index{0}; partition_index < COINS_CACHE_PARTITIONS; ++partition_index) {
        auto& storage{m_impl->cache.m_storage[partition_index]};
        for (auto& record : m_impl->partitions[partition_index].records) {
            if (!record.entry) {
                continue;
            }
            if (record.inserted) {
                storage.coins_usage -= record.entry->second.coin.DynamicMemoryUsage();
                storage.coins.erase(record.outpoint);
            }
        }
    }
    m_impl->cache.m_active_batch = nullptr;
    m_impl->active = false;
    m_impl->Release();
}
