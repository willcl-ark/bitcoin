// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <coinsstore.h>

#include <logging.h>
#include <memusage.h>
#include <util/check.h>
#include <util/threadnames.h>
#include <util/threadpool.h>

#include <algorithm>
#include <array>
#include <bit>
#include <condition_variable>
#include <exception>
#include <functional>
#include <limits>
#include <map>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#ifndef WIN32
#include <sys/mman.h>
#endif

// ---------------------------------------------------------------------------------------------------------
// WindowOverlay (Txid-keyed flat open-addressed 64-shard table)

void WindowOverlay::FlatTable::Rehash(size_t new_cap)
{
    new_cap = std::max<size_t>(16, std::bit_ceil(new_cap));
    std::vector<uint8_t> new_ctrl(new_cap, EMPTY);
    std::vector<TxEntry> new_slots(new_cap);
    const size_t mask{new_cap - 1};
    for (size_t i{0}; i < ctrl.size(); ++i) {
        if (ctrl[i] < 2) continue;
        size_t pos{static_cast<size_t>(slots[i].hash) & mask};
        while (new_ctrl[pos] != EMPTY) pos = (pos + 1) & mask;
        new_ctrl[pos] = ctrl[i];
        new_slots[pos] = slots[i];
    }
    ctrl = std::move(new_ctrl);
    slots = std::move(new_slots);
    tombstones = 0;
}

WindowOverlay::TxEntry* WindowOverlay::FlatTable::Find(const Txid& txid, uint64_t h) noexcept
{
    if (size == 0) return nullptr;
    const size_t mask{ctrl.size() - 1};
    const uint8_t tag{Tag(h)};
    for (size_t pos{static_cast<size_t>(h) & mask};; pos = (pos + 1) & mask) {
        const uint8_t c{ctrl[pos]};
        if (c == EMPTY) return nullptr;
        if (c == tag && slots[pos].hash == h && slots[pos].tx->GetHash() == txid) return &slots[pos];
    }
}

const WindowOverlay::TxEntry* WindowOverlay::FlatTable::Find(const Txid& txid, uint64_t h) const noexcept
{
    if (size == 0) return nullptr;
    const size_t mask{ctrl.size() - 1};
    const uint8_t tag{Tag(h)};
    for (size_t pos{static_cast<size_t>(h) & mask};; pos = (pos + 1) & mask) {
        const uint8_t c{ctrl[pos]};
        if (c == EMPTY) return nullptr;
        if (c == tag && slots[pos].hash == h && slots[pos].tx->GetHash() == txid) return &slots[pos];
    }
}

WindowOverlay::TxEntry* WindowOverlay::FlatTable::TryInsert(const TxEntry& entry)
{
    if (ctrl.empty() || (size + tombstones + 1) * 4 >= ctrl.size() * 3) {
        Rehash(std::max<size_t>(16, std::bit_ceil((size + 1) * 2)));
    }
    const size_t mask{ctrl.size() - 1};
    const uint8_t tag{Tag(entry.hash)};
    const Txid& txid{entry.tx->GetHash()};
    size_t first_tomb{ctrl.size()};
    for (size_t pos{static_cast<size_t>(entry.hash) & mask};; pos = (pos + 1) & mask) {
        const uint8_t c{ctrl[pos]};
        if (c == EMPTY) {
            const size_t dst{first_tomb < ctrl.size() ? first_tomb : pos};
            if (first_tomb < ctrl.size()) --tombstones;
            ctrl[dst] = tag;
            slots[dst] = entry;
            ++size;
            return nullptr;
        }
        if (c == TOMBSTONE) {
            if (first_tomb == ctrl.size()) first_tomb = pos;
        } else if (c == tag && slots[pos].hash == entry.hash && slots[pos].tx->GetHash() == txid) {
            return &slots[pos];
        }
    }
}

bool WindowOverlay::FlatTable::Erase(const Txid& txid, uint64_t h, int32_t height) noexcept
{
    if (size == 0) return false;
    const size_t mask{ctrl.size() - 1};
    const uint8_t tag{Tag(h)};
    for (size_t pos{static_cast<size_t>(h) & mask};; pos = (pos + 1) & mask) {
        const uint8_t c{ctrl[pos]};
        if (c == EMPTY) return false;
        if (c == tag && slots[pos].hash == h && slots[pos].tx->GetHash() == txid) {
            if (slots[pos].height != height) return false;
            ctrl[pos] = TOMBSTONE;
            slots[pos].tx = nullptr;
            --size;
            ++tombstones;
            return true;
        }
    }
}

std::optional<Coin> WindowOverlay::ExtractCoin(const Shard& shard, const TxEntry& entry, uint32_t n)
{
    if (n >= entry.tx->vout.size()) return std::nullopt;
    const CTxOut& out{entry.tx->vout[n]};
    if (out.scriptPubKey.IsUnspendable()) return std::nullopt;
    if (n < 64) {
        if ((entry.intra_spent_mask & (uint64_t{1} << n)) != 0) return std::nullopt;
    } else if (entry.has_high_intra_spend) {
        const COutPoint op{entry.tx->GetHash(), n};
        for (const auto& [hop, hh] : shard.high_intra_spends) {
            if (hh == entry.height && hop == op) return std::nullopt;
        }
    }
    return Coin{out, entry.height, entry.is_coinbase};
}

void WindowOverlay::AddBlockOutputs(int height, std::shared_ptr<const CBlock> block)
{
    if (height == 0 || !block || block->vtx.empty()) return;

    {
        LOCK(m_blocks_mutex);
        m_blocks.insert_or_assign(height, block);
        m_total_blocks.store(m_blocks.size(), std::memory_order_release);
    }

    const size_t n_tx{block->vtx.size()};
    std::vector<TxEntry> entries(n_tx);
    std::vector<uint32_t> shard_of(n_tx);
    std::vector<std::pair<COutPoint, uint32_t>> high_spends;

    // Build a tiny flat hash index of this block's txids (single allocation) to mark intra-block spends.
    const size_t idx_cap{n_tx > 1 ? std::bit_ceil(n_tx * 2) : 0};
    std::vector<uint32_t> tx_index(idx_cap, std::numeric_limits<uint32_t>::max());
    const size_t idx_mask{idx_cap ? idx_cap - 1 : 0};

    for (size_t i{0}; i < n_tx; ++i) {
        const CTransaction* tx{block->vtx[i].get()};
        const Txid& txid{tx->GetHash()};
        const uint64_t h{TxHash(txid)};
        entries[i] = TxEntry{
            .tx = tx,
            .intra_spent_mask = 0,
            .hash = h,
            .height = height,
            .is_coinbase = (i == 0),
            .has_high_intra_spend = false,
        };
        shard_of[i] = static_cast<uint32_t>(ShardIndex(txid));
        if (idx_cap) {
            size_t pos{static_cast<size_t>(h) & idx_mask};
            while (tx_index[pos] != std::numeric_limits<uint32_t>::max()) pos = (pos + 1) & idx_mask;
            tx_index[pos] = static_cast<uint32_t>(i);
        }
    }

    if (idx_cap) {
        for (size_t i{1}; i < n_tx; ++i) {
            for (const CTxIn& in : block->vtx[i]->vin) {
                const uint64_t h{TxHash(in.prevout.hash)};
                for (size_t pos{static_cast<size_t>(h) & idx_mask};; pos = (pos + 1) & idx_mask) {
                    const uint32_t idx{tx_index[pos]};
                    if (idx == std::numeric_limits<uint32_t>::max()) break;
                    if (entries[idx].hash == h && entries[idx].tx->GetHash() == in.prevout.hash) {
                        if (in.prevout.n < 64) {
                            entries[idx].intra_spent_mask |= (uint64_t{1} << in.prevout.n);
                        } else {
                            entries[idx].has_high_intra_spend = true;
                            high_spends.emplace_back(in.prevout, shard_of[idx]);
                        }
                        break;
                    }
                }
            }
        }
    }

    std::array<uint32_t, NUM_SHARDS + 1> starts{};
    for (uint32_t s : shard_of) ++starts[s + 1];
    for (size_t s{0}; s < NUM_SHARDS; ++s) starts[s + 1] += starts[s];
    std::vector<uint32_t> order(n_tx);
    std::array<uint32_t, NUM_SHARDS> next;
    std::copy_n(starts.begin(), NUM_SHARDS, next.begin());
    for (uint32_t i{0}; i < n_tx; ++i) order[next[shard_of[i]]++] = i;

    for (size_t s{0}; s < NUM_SHARDS; ++s) {
        if (starts[s] == starts[s + 1]) continue;
        Shard& shard{m_shards[s]};
        LOCK(shard.mutex);
        for (uint32_t k{starts[s]}; k < starts[s + 1]; ++k) {
            const TxEntry& e{entries[order[k]]};
            if (TxEntry* existing{shard.txs.TryInsert(e)}) {
                if (e.height < existing->height) {
                    shard.dup_txs.push_back(*existing);
                    *existing = e;
                } else {
                    shard.dup_txs.push_back(e);
                }
            }
        }
        if (!high_spends.empty()) {
            for (const auto& [op, hs] : high_spends) {
                if (hs == s) shard.high_intra_spends.emplace_back(op, height);
            }
        }
    }
}

void WindowOverlay::RemoveBlocks(std::span<const int> heights, ThreadPool* pool)
{
    if (heights.empty()) return;

    std::vector<std::pair<int, std::shared_ptr<const CBlock>>> removing;
    removing.reserve(heights.size());
    {
        LOCK(m_blocks_mutex);
        for (const int h : heights) {
            if (auto it{m_blocks.find(h)}; it != m_blocks.end()) {
                removing.emplace_back(h, std::move(it->second));
                m_blocks.erase(it);
            }
        }
        m_total_blocks.store(m_blocks.size(), std::memory_order_release);
    }
    if (removing.empty()) return;

    struct RemItem {
        const Txid* txid;
        uint64_t hash;
        int32_t height;
        uint32_t shard;
    };
    size_t total_txs{0};
    for (const auto& [h, block] : removing) total_txs += block->vtx.size();
    std::vector<RemItem> items;
    items.reserve(total_txs);
    std::array<uint32_t, NUM_SHARDS + 1> starts{};
    for (const auto& [h, block] : removing) {
        for (const CTransactionRef& tx : block->vtx) {
            const Txid& txid{tx->GetHash()};
            const uint32_t s{static_cast<uint32_t>(ShardIndex(txid))};
            items.push_back({&txid, TxHash(txid), h, s});
            ++starts[s + 1];
        }
    }
    if (items.empty()) return;

    for (size_t s{0}; s < NUM_SHARDS; ++s) starts[s + 1] += starts[s];
    std::vector<uint32_t> order(items.size());
    std::array<uint32_t, NUM_SHARDS> next;
    std::copy_n(starts.begin(), NUM_SHARDS, next.begin());
    for (uint32_t i{0}; i < items.size(); ++i) order[next[items[i].shard]++] = i;

    const auto remove_shard{[&](size_t s) noexcept {
        if (starts[s] == starts[s + 1]) return;
        Shard& shard{m_shards[s]};
        LOCK(shard.mutex);
        for (uint32_t k{starts[s]}; k < starts[s + 1]; ++k) {
            const RemItem& item{items[order[k]]};
            if (!shard.dup_txs.empty()) {
                auto dit{std::find_if(shard.dup_txs.begin(), shard.dup_txs.end(), [&](const TxEntry& e) {
                    return e.height == item.height && e.hash == item.hash && e.tx->GetHash() == *item.txid;
                })};
                if (dit != shard.dup_txs.end()) {
                    *dit = shard.dup_txs.back();
                    shard.dup_txs.pop_back();
                    continue;
                }
            }
            if (TxEntry* slot{shard.txs.Find(*item.txid, item.hash)}; slot && slot->height == item.height) {
                if (!shard.dup_txs.empty()) {
                    auto best{shard.dup_txs.end()};
                    for (auto dit{shard.dup_txs.begin()}; dit != shard.dup_txs.end(); ++dit) {
                        if (dit->hash == item.hash && dit->tx->GetHash() == *item.txid &&
                            (best == shard.dup_txs.end() || dit->height < best->height)) {
                            best = dit;
                        }
                    }
                    if (best != shard.dup_txs.end()) {
                        *slot = *best;
                        *best = shard.dup_txs.back();
                        shard.dup_txs.pop_back();
                        continue;
                    }
                }
                shard.txs.Erase(*item.txid, item.hash, item.height);
            }
        }
        if (!shard.high_intra_spends.empty()) {
            std::erase_if(shard.high_intra_spends, [&](const auto& p) {
                return std::ranges::find(heights, p.second) != heights.end();
            });
        }
    }};

    const size_t helpers{pool && items.size() >= 1024 ? std::min<size_t>(pool->WorkersCount(), 8) : 0};
    if (helpers > 0) {
        struct Shared {
            std::atomic<size_t> next{0};
            std::atomic<int> active{0};
        };
        const auto shared{std::make_shared<Shared>()};
        for (size_t h{0}; h < helpers; ++h) {
            (void)pool->Submit([shared, &remove_shard]() noexcept {
                shared->active.fetch_add(1);
                for (size_t s{shared->next.fetch_add(1)}; s < NUM_SHARDS; s = shared->next.fetch_add(1)) {
                    WAITED_FOR_UNDER_LOCK("a window overlay remove helper");
                    remove_shard(s);
                }
                if (shared->active.fetch_sub(1) == 1) shared->active.notify_all();
            }, /*front=*/true);
        }
        for (size_t s{shared->next.fetch_add(1)}; s < NUM_SHARDS; s = shared->next.fetch_add(1)) remove_shard(s);
        for (int active{shared->active.load()}; active != 0; active = shared->active.load()) shared->active.wait(active);
    } else {
        for (size_t s{0}; s < NUM_SHARDS; ++s) remove_shard(s);
    }
}

void WindowOverlay::Clear()
{
    std::unordered_map<int, std::shared_ptr<const CBlock>> blocks;
    {
        LOCK(m_blocks_mutex);
        blocks.swap(m_blocks);
        m_total_blocks.store(0, std::memory_order_release);
    }
    for (Shard& shard : m_shards) {
        LOCK(shard.mutex);
        shard.txs.clear();
        shard.dup_txs.clear();
        shard.high_intra_spends.clear();
    }
}

std::optional<Coin> WindowOverlay::Lookup(int viewer_height, const COutPoint& outpoint) const
{
    if (m_total_blocks.load(std::memory_order_acquire) == 0) return std::nullopt;
    const uint64_t h{TxHash(outpoint.hash)};
    const Shard& shard{m_shards[ShardIndex(outpoint.hash)]};
    LOCK(shard.mutex);
    const TxEntry* entry{shard.txs.Find(outpoint.hash, h)};
    if (!entry || entry->height >= viewer_height) return std::nullopt;
    const TxEntry* best{entry};
    if (!shard.dup_txs.empty()) {
        for (const TxEntry& d : shard.dup_txs) {
            if (d.hash == h && d.tx->GetHash() == outpoint.hash && d.height < viewer_height && d.height > best->height) {
                best = &d;
            }
        }
    }
    return ExtractCoin(shard, *best, outpoint.n);
}

void WindowOverlay::Fetch(int viewer_height, std::span<const COutPoint> outpoints,
                          std::vector<std::pair<COutPoint, Coin>>& found,
                          std::vector<COutPoint>& misses) const
{
    if (outpoints.empty()) return;
    if (m_total_blocks.load(std::memory_order_acquire) == 0) {
        misses.insert(misses.end(), outpoints.begin(), outpoints.end());
        return;
    }
    std::vector<uint32_t> shard_of;
    std::vector<uint64_t> hashes;
    shard_of.reserve(outpoints.size());
    hashes.reserve(outpoints.size());
    std::array<uint32_t, NUM_SHARDS + 1> starts{};
    for (const COutPoint& op : outpoints) {
        const uint32_t s{static_cast<uint32_t>(ShardIndex(op.hash))};
        shard_of.push_back(s);
        hashes.push_back(TxHash(op.hash));
        ++starts[s + 1];
    }
    for (size_t s{0}; s < NUM_SHARDS; ++s) starts[s + 1] += starts[s];
    std::vector<uint32_t> order(outpoints.size());
    std::array<uint32_t, NUM_SHARDS> next;
    std::copy_n(starts.begin(), NUM_SHARDS, next.begin());
    for (uint32_t i{0}; i < outpoints.size(); ++i) order[next[shard_of[i]]++] = i;

    for (size_t s{0}; s < NUM_SHARDS; ++s) {
        if (starts[s] == starts[s + 1]) continue;
        const Shard& shard{m_shards[s]};
        LOCK(shard.mutex);
        if (shard.txs.empty()) {
            for (uint32_t k{starts[s]}; k < starts[s + 1]; ++k) misses.push_back(outpoints[order[k]]);
            continue;
        }
        for (uint32_t k{starts[s]}; k < starts[s + 1]; ++k) {
            const uint32_t idx{order[k]};
            const COutPoint& op{outpoints[idx]};
            const uint64_t h{hashes[idx]};
            const TxEntry* entry{shard.txs.Find(op.hash, h)};
            if (!entry || entry->height >= viewer_height) {
                misses.push_back(op);
                continue;
            }
            const TxEntry* best{entry};
            if (!shard.dup_txs.empty()) {
                for (const TxEntry& d : shard.dup_txs) {
                    if (d.hash == h && d.tx->GetHash() == op.hash && d.height < viewer_height && d.height > best->height) {
                        best = &d;
                    }
                }
            }
            if (auto coin{ExtractCoin(shard, *best, op.n)}) {
                found.emplace_back(op, std::move(*coin));
            } else {
                misses.push_back(op);
            }
        }
    }
}

// ---------------------------------------------------------------------------------------------------------
// RegionAhead & CoinsStore lifecycle

struct CoinsStore::RegionAhead {
    Mutex mutex;
    std::condition_variable cv;
    void* ready GUARDED_BY(mutex){nullptr};
    bool wanted GUARDED_BY(mutex){false};
    bool stop GUARDED_BY(mutex){false};
    int stores GUARDED_BY(mutex){0};
    std::thread thread GUARDED_BY(mutex);

    void Run() EXCLUSIVE_LOCKS_REQUIRED(!mutex)
    {
        util::ThreadRename("coinsahead");
        while (true) {
            {
                WAIT_LOCK(mutex, lock);
                cv.wait(lock, [&]() EXCLUSIVE_LOCKS_REQUIRED(mutex) { return stop || (wanted && !ready); });
                if (stop) return;
                wanted = false;
            }
            void* next{nullptr};
            try {
                next = HintedRegion(NODE_REGION_BYTES, NODE_REGION_BYTES);
            } catch (const std::bad_alloc&) {
                LogWarning("coinsahead: failed to preallocate %zu MiB node region", NODE_REGION_BYTES >> 20);
                continue;
            }
            static constexpr size_t SMALL_PAGE_BYTES{4096};
            for (size_t at{0}; at < NODE_REGION_BYTES; at += SMALL_PAGE_BYTES) static_cast<volatile std::byte*>(next)[at] = std::byte{0};
            LOCK(mutex);
            if (stop) {
                FreeRegion(next, NODE_REGION_BYTES);
                return;
            }
            ready = next;
        }
    }
};

CoinsStore::RegionAhead& CoinsStore::RegionAheadState()
{
    static RegionAhead* const ahead{new RegionAhead};
    return *ahead;
}

CoinsStore::CoinsStore(CCoinsView* base_view, size_t shards, bool deterministic)
    : CCoinsViewCache(base_view, deterministic), m_shards(std::max<size_t>(1, shards))
{
    for (Shard& shard : m_shards) {
        LOCK(shard.mutex);
        Resized(shard);
    }
    RegionAhead& ahead{RegionAheadState()};
    LOCK(ahead.mutex);
    ++ahead.stores;
}

CoinsStore::~CoinsStore()
{
    RegionAhead& ahead{RegionAheadState()};
    std::thread ending;
    {
        LOCK(ahead.mutex);
        if (--ahead.stores != 0 || ahead.stop) return;
        ahead.stop = true;
        ending = std::move(ahead.thread);
    }
    ahead.cv.notify_all();
    if (ending.joinable()) ending.join();
    LOCK(ahead.mutex);
    ahead.stop = false;
    ahead.wanted = false;
    if (void* unused{std::exchange(ahead.ready, nullptr)}) FreeRegion(unused, NODE_REGION_BYTES);
}

// ---------------------------------------------------------------------------------------------------------
// Entry bookkeeping

CoinsStore::Totals& CoinsStore::MyTotals() const
{
    static std::atomic<size_t> next{0};
    static thread_local const size_t stripe{next.fetch_add(1, std::memory_order_relaxed) % TOTALS_STRIPES};
    return m_totals[stripe];
}

size_t CoinsStore::SumTotals(std::atomic<int64_t> Totals::* field) const
{
    int64_t total{0};
    for (const Totals& totals : m_totals) total += (totals.*field).load(std::memory_order_relaxed);
    return static_cast<size_t>(std::max<int64_t>(0, total));
}

void* CoinsStore::HintedRegion(size_t bytes, size_t align)
{
    assert(bytes != 0 && bytes % HUGE_PAGE_BYTES == 0 && align % HUGE_PAGE_BYTES == 0);
    void* region{::operator new(bytes, std::align_val_t{align})};
#ifdef MADV_HUGEPAGE
    (void)madvise(region, bytes, MADV_HUGEPAGE);
#endif
    return region;
}

void* CoinsStore::NodeRegion()
{
    if (NodeRegionBytes() < NODE_REGION_AHEAD_FROM) return HintedRegion(NODE_REGION_BYTES, NODE_REGION_BYTES);
    RegionAhead& ahead{RegionAheadState()};
    void* region;
    {
        LOCK(ahead.mutex);
        if (ahead.stores == 0 || ahead.stop) return HintedRegion(NODE_REGION_BYTES, NODE_REGION_BYTES);
        if (!ahead.thread.joinable()) {
            try {
                ahead.thread = std::thread{[&ahead] { ahead.Run(); }};
            } catch (const std::system_error& e) {
                LogWarning("Failed to start the coinsahead thread (%s)", e.what());
                return HintedRegion(NODE_REGION_BYTES, NODE_REGION_BYTES);
            }
        }
        region = std::exchange(ahead.ready, nullptr);
        ahead.wanted = true;
    }
    ahead.cv.notify_one();
    return region ? region : HintedRegion(NODE_REGION_BYTES, NODE_REGION_BYTES);
}

int64_t CoinsStore::NodeBytes()
{
    return static_cast<int64_t>(sizeof(Map::Cell));
}

size_t CoinsStore::RegionAheadBytes() const
{
    return SumTotals(&Totals::entries) * static_cast<size_t>(NodeBytes()) >= NODE_REGION_AHEAD_FROM ? NODE_REGION_BYTES : 0;
}

size_t CoinsStore::SpanOf(int height)
{
    return static_cast<size_t>(std::max(height, 0)) >> DIRTY_SPAN_BITS;
}

CoinsStore::Bucket& CoinsStore::DirtyBucket(Shard& shard, int height) const
{
    AssertLockHeld(shard.mutex);
    const size_t span{SpanOf(height)};
    if (span >= shard.dirty_spans.size()) {
        const auto before{static_cast<int64_t>(memusage::DynamicUsage(shard.dirty_spans))};
        shard.dirty_spans.resize(span + 1);
        MyTotals().bytes.fetch_add(static_cast<int64_t>(memusage::DynamicUsage(shard.dirty_spans)) - before, std::memory_order_relaxed);
    }
    return shard.dirty_spans[span];
}

void CoinsStore::FreeDirtySpans(Shard& shard) const
{
    AssertLockHeld(shard.mutex);
    MyTotals().bytes.fetch_sub(static_cast<int64_t>(memusage::DynamicUsage(shard.dirty_spans)), std::memory_order_relaxed);
    std::vector<Bucket>{}.swap(shard.dirty_spans);
}

void CoinsStore::AddUsage(Shard& shard, size_t bytes) const
{
    AssertLockHeld(shard.mutex);
    MyTotals().bytes.fetch_add(static_cast<int64_t>(bytes), std::memory_order_relaxed);
}

void CoinsStore::SubUsage(Shard& shard, size_t bytes) const
{
    AssertLockHeld(shard.mutex);
    MyTotals().bytes.fetch_sub(static_cast<int64_t>(bytes), std::memory_order_relaxed);
}

void CoinsStore::Resized(Shard& shard) const
{
    AssertLockHeld(shard.mutex);
    const size_t entries{shard.map.size()};
    const size_t buckets{shard.map.bucket_count()};
    if (entries == shard.entries && buckets == shard.buckets) return;
    const int64_t delta{static_cast<int64_t>(entries) - static_cast<int64_t>(shard.entries)};
    int64_t bytes{NodeBytes() * delta};
    if (buckets != shard.buckets) {
        bytes += static_cast<int64_t>(memusage::MallocUsage(Map::BUCKET_BYTES * buckets)) - static_cast<int64_t>(memusage::MallocUsage(Map::BUCKET_BYTES * shard.buckets));
    }
    Totals& totals{MyTotals()};
    totals.entries.fetch_add(delta, std::memory_order_relaxed);
    totals.bytes.fetch_add(bytes, std::memory_order_relaxed);
    shard.entries = entries;
    shard.buckets = buckets;
}

int64_t CoinsStore::DirtyBytes(const Entry& entry)
{
    return NodeBytes() + static_cast<int64_t>(entry.coin.DynamicMemoryUsage());
}

void CoinsStore::Detach(Shard& shard, Node& node) const
{
    AssertLockHeld(shard.mutex);
    Entry& entry{node.second};
    if (entry.list == LIST_DIRTY) {
        Bucket& bucket{shard.dirty_spans[SpanOf(entry.height)]};
        if (entry.prev) entry.prev->second.next = entry.next; else bucket.head = entry.next;
        if (entry.next) entry.next->second.prev = entry.prev;
        bucket.bytes -= DirtyBytes(entry);
        Assume(bucket.head || bucket.bytes == 0);
        Assert(shard.dirty_count > 0);
        --shard.dirty_count;
        MyTotals().dirty.fetch_sub(1, std::memory_order_relaxed);
        if (shard.dirty_count == 0) FreeDirtySpans(shard);
    } else if (entry.list == LIST_CLEAN) {
        if (entry.prev) entry.prev->second.next = entry.next; else shard.clean_young = entry.next;
        if (entry.next) entry.next->second.prev = entry.prev; else shard.clean_old = entry.prev;
    }
    entry.list = LIST_NONE;
    entry.prev = nullptr;
    entry.next = nullptr;
}

void CoinsStore::PrefetchDetach(const Shard& shard, const Node& node) const
{
    AssertLockHeld(shard.mutex);
    Map::PrefetchLinks(node);
    if (node.second.list == LIST_DIRTY) Map::PrefetchWrite(shard.dirty_spans.data() + SpanOf(node.second.height));
}

void CoinsStore::Attach(Shard& shard, Node& node) const
{
    AssertLockHeld(shard.mutex);
    Entry& entry{node.second};
    Assume(entry.list == LIST_NONE);
    if (entry.dirty) {
        Bucket& bucket{DirtyBucket(shard, entry.height)};
        entry.prev = nullptr;
        entry.next = bucket.head;
        if (bucket.head) bucket.head->second.prev = &node;
        bucket.head = &node;
        bucket.bytes += DirtyBytes(entry);
        ++shard.dirty_count;
        MyTotals().dirty.fetch_add(1, std::memory_order_relaxed);
        entry.list = LIST_DIRTY;
    } else {
        entry.prev = nullptr;
        entry.next = shard.clean_young;
        if (shard.clean_young) shard.clean_young->second.prev = &node; else shard.clean_old = &node;
        shard.clean_young = &node;
        entry.list = LIST_CLEAN;
    }
}

void CoinsStore::EraseDetached(Shard& shard, Map::iterator it) const
{
    AssertLockHeld(shard.mutex);
    Assume(it->second.list == LIST_NONE);
    Assert(!it->second.collected);
    shard.map.erase(it);
    Resized(shard);
}

void CoinsStore::EraseEntry(Shard& shard, Map::iterator it) const
{
    AssertLockHeld(shard.mutex);
    Entry& entry{it->second};
    Detach(shard, *it);
    SubUsage(shard, entry.coin.DynamicMemoryUsage());
    EraseDetached(shard, it);
}

void CoinsStore::EraseEntry(Shard& shard, Node& node) const
{
    AssertLockHeld(shard.mutex);
    Entry& entry{node.second};
    Detach(shard, node);
    SubUsage(shard, entry.coin.DynamicMemoryUsage());
    Assert(!entry.collected);
    const COutPoint outpoint{node.first};
    shard.map.erase(outpoint);
    Resized(shard);
}

// ---------------------------------------------------------------------------------------------------------
// Lookups

std::optional<Coin> CoinsStore::Lookup(const COutPoint& outpoint) const
{
    Shard& shard{ShardFor(outpoint)};
    while (true) {
        uint64_t settles;
        {
            LOCK(shard.mutex);
            if (auto it{shard.map.find(outpoint)}; it != shard.map.end()) {
                Entry& entry{it->second};
                if (entry.coin.IsSpent()) return std::nullopt;
                Reference(entry);
                return entry.coin.ToCoin();
            }
            settles = shard.settles;
        }
        std::optional<Coin> coin{base->GetCoin(outpoint)};
        LOCK(shard.mutex);
        if (shard.settles != settles) continue;
        auto it{shard.map.find(outpoint)};
        if (it != shard.map.end()) {
            Entry& entry{it->second};
            if (entry.coin.IsSpent()) return std::nullopt;
            Reference(entry);
            return entry.coin.ToCoin();
        }
        if (!coin) return std::nullopt;
        Assert(!coin->IsSpent());
        it = shard.map.try_emplace(outpoint).first;
        Resized(shard);
        it->second.coin = *coin;
        Transit(it->second, coins_entry::ABSENT, Event::BaseCopy, static_cast<int>(coin->nHeight));
        AddUsage(shard, it->second.coin.DynamicMemoryUsage());
        Attach(shard, *it);
        return coin;
    }
}

std::optional<Coin> CoinsStore::GetCoin(const COutPoint& outpoint) const
{
    return Lookup(outpoint);
}

std::optional<Coin> CoinsStore::PeekCoin(const COutPoint& outpoint) const
{
    Shard& shard{ShardFor(outpoint)};
    while (true) {
        uint64_t settles;
        {
            LOCK(shard.mutex);
            if (auto it{shard.map.find(outpoint)}; it != shard.map.end()) {
                Entry& entry{it->second};
                if (entry.coin.IsSpent()) return std::nullopt;
                Reference(entry);
                return entry.coin.ToCoin();
            }
            settles = shard.settles;
        }
        std::optional<Coin> coin{base->PeekCoin(outpoint)};
        if (!coin) return std::nullopt;
        LOCK(shard.mutex);
        if (shard.settles != settles) continue;
        if (auto it{shard.map.find(outpoint)}; it != shard.map.end()) {
            Entry& entry{it->second};
            if (entry.coin.IsSpent()) return std::nullopt;
            Reference(entry);
            return entry.coin.ToCoin();
        }
        return coin;
    }
}

bool CoinsStore::HaveCoin(const COutPoint& outpoint) const
{
    return Lookup(outpoint).has_value();
}

bool CoinsStore::HaveCoinInCache(const COutPoint& outpoint) const
{
    Shard& shard{ShardFor(outpoint)};
    LOCK(shard.mutex);
    const auto it{shard.map.find(outpoint)};
    return it != shard.map.end() && !it->second.coin.IsSpent();
}

static const Coin STORE_EMPTY_COIN;

const Coin& CoinsStore::AccessCoin(const COutPoint& outpoint) const
{
    std::optional<Coin> coin{Lookup(outpoint)};
    if (!coin) return STORE_EMPTY_COIN;
    static thread_local Coin t_coin;
    t_coin = std::move(*coin);
    return t_coin;
}

uint256 CoinsStore::GetBestBlock() const
{
    if (m_tier_block_hash.IsNull()) {
        m_tier_block_hash = base->GetBestBlock();
    }
    return m_tier_block_hash;
}

void CoinsStore::SetBestBlock(const uint256& block_hash)
{
    m_tier_block_hash = block_hash;
}

void CoinsStore::SetBestBlock(const uint256& block_hash, int height)
{
    m_tier_block_hash = block_hash;
    m_change_height = height;
    SetTierHeight(height);
}

// ---------------------------------------------------------------------------------------------------------
// Confirmed mutations

void CoinsStore::KeepWritten(std::vector<CoinWrite> restored)
{
    for (const CoinWrite& write : restored) {
        Shard& shard{ShardFor(write.outpoint)};
        LOCK(shard.mutex);
        if (auto it{shard.map.find(write.outpoint)}; it != shard.map.end()) Transit(it->second, Event::KeptWritten);
    }
    if (m_restored.empty()) {
        m_restored = std::move(restored);
        return;
    }
    m_restored.insert(m_restored.end(), std::make_move_iterator(restored.begin()), std::make_move_iterator(restored.end()));
}

void CoinsStore::AddCoin(const COutPoint& outpoint, Coin&& coin, bool possible_overwrite)
{
    assert(!coin.IsSpent());
    if (coin.out.scriptPubKey.IsUnspendable()) return;
    Shard& shard{ShardFor(outpoint)};
    LOCK(shard.mutex);
    auto [it, inserted]{shard.map.try_emplace(outpoint)};
    if (inserted) Resized(shard);
    Entry& entry{it->second};
    const State from{StateOf(entry)};
    bool fresh{false};
    if (!possible_overwrite) {
        if (!entry.coin.IsSpent()) {
            throw std::logic_error("Attempted to overwrite an unspent coin (when possible_overwrite is false)");
        }
        fresh = inserted;
    }
    if (!inserted) {
        Detach(shard, *it);
        SubUsage(shard, entry.coin.DynamicMemoryUsage());
    }
    entry.coin = std::move(coin);
    Transit(entry, from, fresh ? Event::ConfirmedNew : Event::ConfirmedWrite, TagFor(entry.coin));
    AddUsage(shard, entry.coin.DynamicMemoryUsage());
    Attach(shard, *it);
}

void CoinsStore::EmplaceCoinInternalDANGER(const COutPoint& outpoint, Coin&& coin)
{
    Shard& shard{ShardFor(outpoint)};
    LOCK(shard.mutex);
    auto [it, inserted]{shard.map.try_emplace(outpoint)};
    if (inserted) Resized(shard);
    if (!inserted) return;
    Entry& entry{it->second};
    entry.coin = std::move(coin);
    Transit(entry, coins_entry::ABSENT, entry.coin.IsSpent() ? Event::ConfirmedSpend : Event::ConfirmedWrite, m_change_height);
    AddUsage(shard, entry.coin.DynamicMemoryUsage());
    Attach(shard, *it);
}

bool CoinsStore::SpendEntry(Shard& shard, Map::iterator it, Coin* moveto)
{
    AssertLockHeld(shard.mutex);
    Entry& entry{it->second};
    if (entry.coin.IsSpent()) {
        if (moveto) *moveto = Coin{};
        return true;
    }
    const State from{StateOf(entry)};
    Detach(shard, *it);
    SubUsage(shard, entry.coin.DynamicMemoryUsage());
    if (moveto) *moveto = entry.coin.ToCoin();
    if (entry.fresh) {
        EraseDetached(shard, it);
        return true;
    }
    entry.coin.Clear();
    Transit(entry, from, Event::ConfirmedSpend, m_change_height);
    Attach(shard, *it);
    return true;
}

bool CoinsStore::SpendCoin(const COutPoint& outpoint, Coin* moveto)
{
    Shard& shard{ShardFor(outpoint)};
    while (true) {
        uint64_t settles;
        {
            LOCK(shard.mutex);
            if (auto it{shard.map.find(outpoint)}; it != shard.map.end()) return SpendEntry(shard, it, moveto);
            settles = shard.settles;
        }
        std::optional<Coin> coin{base->GetCoin(outpoint)};
        LOCK(shard.mutex);
        if (shard.settles != settles) continue;
        auto it{shard.map.find(outpoint)};
        if (it != shard.map.end()) return SpendEntry(shard, it, moveto);
        if (!coin) return false;
        if (moveto) *moveto = std::move(*coin);
        it = shard.map.try_emplace(outpoint).first;
        Resized(shard);
        Transit(it->second, coins_entry::ABSENT, Event::ConfirmedSpend, m_change_height);
        Attach(shard, *it);
        return true;
    }
}

void CoinsStore::Uncache(const COutPoint& outpoint)
{
    Shard& shard{ShardFor(outpoint)};
    LOCK(shard.mutex);
    auto it{shard.map.find(outpoint)};
    if (it != shard.map.end() && !it->second.dirty) {
        EraseEntry(shard, it);
    }
}

void CoinsStore::BatchWrite(CoinsViewCacheCursor& cursor, const uint256& block_hash)
{
    for (auto it{cursor.Begin()}; it != cursor.End(); it = cursor.NextAndMaybeErase(*it)) {
        if (!it->second.IsDirty()) continue;
        const COutPoint& outpoint{it->first};
        CCoinsCacheEntry& child{it->second};
        Shard& shard{ShardFor(outpoint)};
        LOCK(shard.mutex);
        auto map_it{shard.map.find(outpoint)};
        if (map_it == shard.map.end()) {
            if (child.IsFresh() && child.coin.IsSpent()) continue;
            map_it = shard.map.try_emplace(outpoint).first;
            Resized(shard);
            Entry& entry{map_it->second};
            if (child.coin.IsSpent()) {
                Transit(entry, coins_entry::ABSENT, Event::ConfirmedSpend, m_change_height);
            } else {
                entry.coin = std::move(child.coin);
                Transit(entry, coins_entry::ABSENT, child.IsFresh() ? Event::ConfirmedNew : Event::ConfirmedWrite, TagFor(entry.coin));
                AddUsage(shard, entry.coin.DynamicMemoryUsage());
            }
            Attach(shard, *map_it);
            continue;
        }
        Entry& entry{map_it->second};
        if (child.IsFresh() && !entry.coin.IsSpent()) {
            throw std::logic_error("Fresh flag misapplied to coin that exists in parent cache");
        }
        if (entry.fresh && child.coin.IsSpent()) {
            EraseEntry(shard, map_it);
            continue;
        }
        const State from{StateOf(entry)};
        Detach(shard, *map_it);
        SubUsage(shard, entry.coin.DynamicMemoryUsage());
        if (child.coin.IsSpent()) {
            entry.coin.Clear();
            Transit(entry, from, Event::ConfirmedSpend, m_change_height);
        } else {
            entry.coin = std::move(child.coin);
            Transit(entry, from, Event::BatchOverwrite, TagFor(entry.coin));
            AddUsage(shard, entry.coin.DynamicMemoryUsage());
        }
        Attach(shard, *map_it);
    }
    if (!block_hash.IsNull()) SetBestBlock(block_hash);
}

// ---------------------------------------------------------------------------------------------------------
// Shard iteration, read-only FetchConfirmed & shard-parallel CommitBatch

template <typename Work>
void CoinsStore::VisitShards(size_t start_hint, const std::vector<uint32_t>& shard_of, Work work) const
{
    const size_t shards{m_shards.size()};
    std::vector<uint32_t> starts(shards + 1, 0);
    for (const uint32_t s : shard_of) ++starts[s + 1];
    for (size_t s{0}; s < shards; ++s) starts[s + 1] += starts[s];
    std::vector<uint32_t> order(shard_of.size());
    {
        std::vector<uint32_t> next(starts.begin(), starts.end() - 1);
        for (uint32_t i{0}; i < shard_of.size(); ++i) order[next[shard_of[i]]++] = i;
    }
    const size_t first{start_hint % shards};
    std::vector<uint32_t> deferred;
    for (size_t n{0}, s{first}; n < shards; ++n, s = s + 1 == shards ? 0 : s + 1) {
        if (starts[s] == starts[s + 1]) continue;
        Shard& shard{m_shards[s]};
        TRY_LOCK(shard.mutex, locked);
        if (!locked) {
            deferred.push_back(static_cast<uint32_t>(s));
            continue;
        }
        work(shard, std::span<const uint32_t>{order}.subspan(starts[s], starts[s + 1] - starts[s]));
    }
    for (const uint32_t s : deferred) {
        Shard& shard{m_shards[s]};
        LOCK(shard.mutex);
        work(shard, std::span<const uint32_t>{order}.subspan(starts[s], starts[s + 1] - starts[s]));
    }
}

template <typename Work>
void CoinsStore::VisitShardsShared(const std::vector<uint32_t>& shard_of, ThreadPool& pool, size_t helpers, Work work) const
{
    const size_t shards{m_shards.size()};
    std::vector<uint32_t> starts(shards + 1, 0);
    for (const uint32_t s : shard_of) ++starts[s + 1];
    for (size_t s{0}; s < shards; ++s) starts[s + 1] += starts[s];
    std::vector<uint32_t> order(shard_of.size());
    {
        std::vector<uint32_t> next(starts.begin(), starts.end() - 1);
        for (uint32_t i{0}; i < shard_of.size(); ++i) order[next[shard_of[i]]++] = i;
    }
    std::vector<uint32_t> taken;
    taken.reserve(shards);
    for (size_t s{0}; s < shards; ++s) {
        if (starts[s] != starts[s + 1]) taken.push_back(static_cast<uint32_t>(s));
    }
    if (taken.empty()) return;

    struct Shared {
        std::atomic<size_t> next{0};
        std::atomic<int> active{0};
        size_t count{0};
    };
    const auto shared{std::make_shared<Shared>()};
    shared->count = taken.size();

    // noexcept: work mutates in-memory shards; any allocation failure terminates rather than leaving
    // cs_main deadlocked or helpers referencing an unwound caller stack.
    [&]() noexcept {
        const auto take{[&](size_t i) noexcept {
            const uint32_t s{taken[i]};
            Shard& shard{m_shards[s]};
            LOCK(shard.mutex);
            work(shard, std::span<const uint32_t>{order}.subspan(starts[s], starts[s + 1] - starts[s]));
        }};
        const size_t num_helpers{std::min(helpers, taken.size() - 1)};
        if (num_helpers > 0) {
            for (size_t h{0}; h < num_helpers; ++h) {
                (void)pool.Submit([shared, take = &take]() noexcept {
                    shared->active.fetch_add(1);
                    for (size_t i{shared->next.fetch_add(1)}; i < shared->count; i = shared->next.fetch_add(1)) {
                        WAITED_FOR_UNDER_LOCK("a shard commit helper");
                        (*take)(i);
                    }
                    if (shared->active.fetch_sub(1) == 1) shared->active.notify_all();
                }, /*front=*/true);
            }
        }
        for (size_t i{shared->next.fetch_add(1)}; i < shared->count; i = shared->next.fetch_add(1)) take(i);
        for (int active{shared->active.load()}; active != 0; active = shared->active.load()) shared->active.wait(active);
    }();
}

std::vector<std::pair<COutPoint, Coin>> CoinsStore::FetchConfirmed(std::vector<COutPoint>&& outpoints) const
{
    std::vector<std::pair<COutPoint, Coin>> found;
    if (outpoints.empty()) return found;
    found.reserve(outpoints.size());
    std::vector<uint32_t> shard_of;
    std::vector<uint64_t> hashes;
    shard_of.reserve(outpoints.size());
    hashes.reserve(outpoints.size());
    for (const COutPoint& outpoint : outpoints) {
        shard_of.push_back(static_cast<uint32_t>(ShardIndex(outpoint)));
        hashes.push_back(KeyHash(outpoint));
    }
    // Use first outpoint's hash to spread start shard across workers.
    const size_t start_hint{static_cast<size_t>(hashes[0])};
    VisitShards(start_hint, shard_of, [&](Shard& shard, std::span<const uint32_t> items) EXCLUSIVE_LOCKS_REQUIRED(shard.mutex) {
        shard.map.PrefetchAll(hashes, items, [](uint32_t) { return true; });
        for (const uint32_t i : items) {
            const auto it{shard.map.find(outpoints[i], hashes[i])};
            if (it == shard.map.end() || it->second.coin.IsSpent()) continue;
            Reference(it->second);
            found.emplace_back(outpoints[i], it->second.coin.ToCoin());
        }
    });
    return found;
}

CoinsStore::BatchCheckResult CoinsStore::CheckBatchConflicts(std::span<BlockChanges*> blocks, ThreadPool* pool) const
{
    if (blocks.empty()) return {.accepted = 0, .conflicts = 0};
    const size_t num_shards{m_shards.size()};
    size_t total_ops{0};
    for (const BlockChanges* b : blocks) total_ops += b->ops.size();
    if (total_ops == 0) return {.accepted = blocks.size(), .conflicts = 0};

    const int written_height{m_written_height};
    const uint64_t current_epoch{m_write_out_epoch.load(std::memory_order_acquire)};
    std::atomic<size_t> first_conflict{blocks.size()};
    std::atomic<size_t> conflict_count{0};

    struct Ref {
        uint64_t hash;
        uint32_t block_idx;
        uint32_t op_idx;
    };

    const auto check_shard{[&](size_t s) noexcept {
        thread_local std::vector<Ref> refs;
        refs.clear();
        for (uint32_t bi{0}; bi < blocks.size(); ++bi) {
            const BlockChanges& b{*blocks[bi]};
            if (b.shard_offsets.empty()) continue;
            const uint32_t begin{b.shard_offsets[s]};
            const uint32_t end{b.shard_offsets[s + 1]};
            for (uint32_t oi{begin}; oi < end; ++oi) {
                refs.push_back({b.ops[oi].key_hash, bi, oi});
            }
        }
        if (refs.empty()) return;

        if (blocks.size() > 1 && refs.size() > 1) {
            std::sort(refs.begin(), refs.end(), [](const Ref& a, const Ref& b) {
                return a.hash != b.hash ? a.hash < b.hash : a.block_idx < b.block_idx;
            });
        }

        const Shard& shard{m_shards[s]};
        LOCK(shard.mutex);
        for (const Ref& r : refs) shard.map.Prefetch(r.hash);
        for (const Ref& r : refs) {
            const CommitOp& op{blocks[r.block_idx]->ops[r.op_idx]};
            if (op.coin.IsSpent() || blocks[r.block_idx]->must_be_new) {
                shard.map.PrefetchNode(r.hash, /*whole=*/false);
            }
        }

        auto record_conflict{[&](size_t bi) {
            size_t prev{first_conflict.load(std::memory_order_relaxed)};
            while (bi < prev && !first_conflict.compare_exchange_weak(prev, bi, std::memory_order_relaxed)) {}
            conflict_count.fetch_add(1, std::memory_order_relaxed);
        }};

        size_t i{0};
        while (i < refs.size()) {
            const size_t limit{first_conflict.load(std::memory_order_relaxed)};
            size_t j{i + 1};
            if (blocks.size() > 1) {
                while (j < refs.size() && refs[j].hash == refs[i].hash) ++j;
            }
            if (j - i == 1) {
                // Fast path (95%+): unique key hash in this shard for the entire batch.
                const Ref& r{refs[i]};
                if (r.block_idx < limit) {
                    const BlockChanges& b{*blocks[r.block_idx]};
                    const CommitOp& op{b.ops[r.op_idx]};
                    if (op.coin.IsSpent()) {
                        const auto it{shard.map.find(op.outpoint, r.hash)};
                        if (it != shard.map.end()) {
                            if (it->second.coin.IsSpent()) record_conflict(r.block_idx);
                        } else if (op.spent_height > written_height) {
                            record_conflict(r.block_idx);
                        } else if (b.validate_epoch != current_epoch && !base->HaveCoin(op.outpoint)) {
                            record_conflict(r.block_idx);
                        }
                    } else if (b.must_be_new) {
                        const auto it{shard.map.find(op.outpoint, r.hash)};
                        if (it != shard.map.end()) {
                            if (!it->second.coin.IsSpent()) record_conflict(r.block_idx);
                        } else if (b.validate_epoch != current_epoch && base->HaveCoin(op.outpoint)) {
                            record_conflict(r.block_idx);
                        }
                    }
                }
                i = j;
                continue;
            }

            // Multiple ops share the same 64-bit hash: group by exact COutPoint.
            for (size_t a{i}; a < j; ++a) {
                const COutPoint& target{blocks[refs[a].block_idx]->ops[refs[a].op_idx].outpoint};
                bool already_done{false};
                for (size_t prev{i}; prev < a; ++prev) {
                    if (blocks[refs[prev].block_idx]->ops[refs[prev].op_idx].outpoint == target) {
                        already_done = true;
                        break;
                    }
                }
                if (already_done) continue;

                enum class Sim : uint8_t { Absent, Unspent, Spent };
                const auto it{shard.map.find(target, refs[a].hash)};
                Sim sim{it == shard.map.end() ? Sim::Absent : (it->second.coin.IsSpent() ? Sim::Spent : Sim::Unspent)};
                std::optional<Ref> live_fresh_create;

                for (size_t k{a}; k < j; ++k) {
                    const Ref& r{refs[k]};
                    if (r.block_idx >= first_conflict.load(std::memory_order_relaxed)) break;
                    BlockChanges& b{*blocks[r.block_idx]};
                    CommitOp& op{b.ops[r.op_idx]};
                    if (!(op.outpoint == target)) continue;
                    if (op.coin.IsSpent()) {
                        if (sim == Sim::Spent) {
                            record_conflict(r.block_idx);
                            break;
                        }
                        if (sim == Sim::Absent && (op.spent_height > written_height || (b.validate_epoch != current_epoch && !base->HaveCoin(target)))) {
                            record_conflict(r.block_idx);
                            break;
                        }
                        if (live_fresh_create) {
                            blocks[live_fresh_create->block_idx]->ops[live_fresh_create->op_idx].cancelled = true;
                            op.cancelled = true;
                            live_fresh_create.reset();
                            sim = Sim::Spent;
                        } else {
                            sim = Sim::Spent;
                        }
                    } else {
                        if (b.must_be_new) {
                            if (sim == Sim::Unspent || (sim == Sim::Absent && b.validate_epoch != current_epoch && base->HaveCoin(target))) {
                                record_conflict(r.block_idx);
                                break;
                            }
                        }
                        if (sim == Sim::Absent && !op.possible_overwrite) {
                            live_fresh_create = r;
                        } else {
                            live_fresh_create.reset();
                        }
                        sim = Sim::Unspent;
                    }
                }
            }
            i = j;
        }
    }};

    const size_t helpers{pool && total_ops >= COMMIT_OPS_PER_HELPER
                             ? std::min(pool->WorkersCount(), total_ops / COMMIT_OPS_PER_HELPER)
                             : 0};
    [&]() noexcept {
        if (helpers > 0) {
            struct Shared {
                std::atomic<size_t> next{0};
                std::atomic<int> active{0};
                size_t count{0};
            };
            const auto shared{std::make_shared<Shared>()};
            shared->count = num_shards;
            for (size_t h{0}; h < helpers; ++h) {
                (void)pool->Submit([shared, &check_shard]() noexcept {
                    shared->active.fetch_add(1);
                    for (size_t s{shared->next.fetch_add(1)}; s < shared->count; s = shared->next.fetch_add(1)) {
                        WAITED_FOR_UNDER_LOCK("a batch conflict check helper");
                        check_shard(s);
                    }
                    if (shared->active.fetch_sub(1) == 1) shared->active.notify_all();
                }, /*front=*/true);
            }
            for (size_t s{shared->next.fetch_add(1)}; s < shared->count; s = shared->next.fetch_add(1)) check_shard(s);
            for (int active{shared->active.load()}; active != 0; active = shared->active.load()) shared->active.wait(active);
        } else {
            for (size_t s{0}; s < num_shards; ++s) check_shard(s);
        }
    }();

    const size_t accepted{first_conflict.load(std::memory_order_relaxed)};
    if (accepted < blocks.size()) {
        // Clear any cross-boundary cancellations into rejected blocks so the accepted prefix commits cleanly.
        for (size_t bi{0}; bi < accepted; ++bi) {
            for (CommitOp& op : blocks[bi]->ops) op.cancelled = false;
        }
    }
    return {.accepted = accepted, .conflicts = conflict_count.load(std::memory_order_relaxed)};
}

void CoinsStore::ApplyCheckedBlocks(std::span<BlockChanges*> blocks, const uint256& best_hash, int best_height, ThreadPool* pool, bool publish_height)
{
    Assert(!m_run_in_flight);
    const size_t num_shards{m_shards.size()};
    size_t total_ops{0};
    for (const BlockChanges* b : blocks) total_ops += b->ops.size();

    if (total_ops > 0) {
        const auto apply_shard{[&](size_t s) noexcept {
            Shard& shard{m_shards[s]};
            bool any{false};
            for (const BlockChanges* b : blocks) {
                if (!b->shard_offsets.empty() && b->shard_offsets[s] < b->shard_offsets[s + 1]) {
                    any = true;
                    break;
                }
            }
            if (!any) return;

            LOCK(shard.mutex);
            // Prefetch buckets, nodes, and detach links for all active ops in this shard.
            for (const BlockChanges* b : blocks) {
                if (b->shard_offsets.empty()) continue;
                const uint32_t end{b->shard_offsets[s + 1]};
                for (uint32_t oi{b->shard_offsets[s]}; oi < end; ++oi) {
                    const CommitOp& op{b->ops[oi]};
                    if (!op.cancelled) shard.map.Prefetch(op.key_hash);
                }
            }
            for (const BlockChanges* b : blocks) {
                if (b->shard_offsets.empty()) continue;
                const uint32_t end{b->shard_offsets[s + 1]};
                for (uint32_t oi{b->shard_offsets[s]}; oi < end; ++oi) {
                    const CommitOp& op{b->ops[oi]};
                    if (!op.cancelled) shard.map.PrefetchNode(op.key_hash, op.coin.IsSpent());
                }
            }

            // Apply in exact block height order.
            for (BlockChanges* b : blocks) {
                if (b->shard_offsets.empty()) continue;
                const uint32_t end{b->shard_offsets[s + 1]};
                for (uint32_t oi{b->shard_offsets[s]}; oi < end; ++oi) {
                    CommitOp& op{b->ops[oi]};
                    if (op.cancelled) continue;
                    if (op.coin.IsSpent()) {
                        auto it{shard.map.find(op.outpoint, op.key_hash)};
                        if (it != shard.map.end()) {
                            Entry& entry{it->second};
                            const State from{StateOf(entry)};
                            Detach(shard, *it);
                            SubUsage(shard, entry.coin.DynamicMemoryUsage());
                            if (entry.fresh) {
                                EraseDetached(shard, it);
                            } else {
                                entry.coin.Clear();
                                Transit(entry, from, Event::ConfirmedSpend, op.height);
                                Attach(shard, *it);
                            }
                        } else {
                            auto [ins, inserted]{shard.map.try_emplace(op.outpoint, op.key_hash)};
                            Assert(inserted);
                            Resized(shard);
                            Transit(ins->second, coins_entry::ABSENT, Event::ConfirmedSpend, op.height);
                            Attach(shard, *ins);
                        }
                    } else {
                        auto [it, inserted]{shard.map.try_emplace(op.outpoint, op.key_hash)};
                        Entry& entry{it->second};
                        const State from{StateOf(entry)};
                        if (inserted) {
                            Resized(shard);
                            entry.coin = std::move(op.coin);
                            Transit(entry, from, op.possible_overwrite ? Event::ConfirmedWrite : Event::ConfirmedNew, op.height);
                            AddUsage(shard, entry.coin.DynamicMemoryUsage());
                            Attach(shard, *it);
                        } else {
                            Assert(op.possible_overwrite || entry.coin.IsSpent());
                            Detach(shard, *it);
                            SubUsage(shard, entry.coin.DynamicMemoryUsage());
                            entry.coin = std::move(op.coin);
                            Transit(entry, from, Event::ConfirmedWrite, op.height);
                            AddUsage(shard, entry.coin.DynamicMemoryUsage());
                            Attach(shard, *it);
                        }
                    }
                }
            }
        }};

        const size_t helpers{pool && total_ops >= COMMIT_OPS_PER_HELPER
                                 ? std::min(pool->WorkersCount(), total_ops / COMMIT_OPS_PER_HELPER)
                                 : 0};
        [&]() noexcept {
            if (helpers > 0) {
                struct Shared {
                    std::atomic<size_t> next{0};
                    std::atomic<int> active{0};
                    size_t count{0};
                };
                const auto shared{std::make_shared<Shared>()};
                shared->count = num_shards;
                for (size_t h{0}; h < helpers; ++h) {
                    (void)pool->Submit([shared, &apply_shard]() noexcept {
                        shared->active.fetch_add(1);
                        for (size_t s{shared->next.fetch_add(1)}; s < shared->count; s = shared->next.fetch_add(1)) {
                            WAITED_FOR_UNDER_LOCK("a shard commit helper");
                            apply_shard(s);
                        }
                        if (shared->active.fetch_sub(1) == 1) shared->active.notify_all();
                    }, /*front=*/true);
                }
                for (size_t s{shared->next.fetch_add(1)}; s < shared->count; s = shared->next.fetch_add(1)) apply_shard(s);
                for (int active{shared->active.load()}; active != 0; active = shared->active.load()) shared->active.wait(active);
            } else {
                for (size_t s{0}; s < num_shards; ++s) apply_shard(s);
            }
        }();
    }

    m_tier_block_hash = best_hash;
    m_change_height = best_height;
    m_tier_height = best_height;
    if (publish_height) m_confirmed_height.store(best_height, std::memory_order_release);
}

void CoinsStore::CommitBatch(std::span<CommitOp> ops, const uint256& best_hash, int best_height, ThreadPool* pool, bool publish_height)
{
    Assert(!m_run_in_flight);
    std::vector<uint32_t> active_idx;
    std::vector<uint32_t> shard_of;
    std::vector<uint64_t> hashes;
    active_idx.reserve(ops.size());
    shard_of.reserve(ops.size());
    hashes.reserve(ops.size());
    for (uint32_t i{0}; i < ops.size(); ++i) {
        if (ops[i].cancelled) continue;
        active_idx.push_back(i);
        shard_of.push_back(ops[i].shard);
        hashes.push_back(ops[i].key_hash);
    }

    if (!active_idx.empty()) {
        const auto apply_shard{[&](Shard& shard, std::span<const uint32_t> items) EXCLUSIVE_LOCKS_REQUIRED(shard.mutex) {
            shard.map.PrefetchAll(hashes, items, [&](uint32_t pos) { return ops[active_idx[pos]].coin.IsSpent(); });
            // Prefetch detach links for spends before applying.
            for (const uint32_t pos : items) {
                const CommitOp& op{ops[active_idx[pos]]};
                if (!op.coin.IsSpent()) continue;
                if (const auto it{shard.map.find(op.outpoint, op.key_hash)}; it != shard.map.end()) PrefetchDetach(shard, *it);
            }
            for (const uint32_t pos : items) {
                CommitOp& op{ops[active_idx[pos]]};
                if (op.coin.IsSpent()) {
                    auto it{shard.map.find(op.outpoint, op.key_hash)};
                    if (it != shard.map.end()) {
                        Entry& entry{it->second};
                        const State from{StateOf(entry)};
                        Detach(shard, *it);
                        SubUsage(shard, entry.coin.DynamicMemoryUsage());
                        if (entry.fresh) {
                            EraseDetached(shard, it);
                        } else {
                            entry.coin.Clear();
                            Transit(entry, from, Event::ConfirmedSpend, op.height);
                            Attach(shard, *it);
                        }
                    } else {
                        // Validated coin from LevelDB not cached in CoinsStore: insert dirty spent marker.
                        auto [ins, inserted]{shard.map.try_emplace(op.outpoint, op.key_hash)};
                        Assert(inserted);
                        Resized(shard);
                        Transit(ins->second, coins_entry::ABSENT, Event::ConfirmedSpend, op.height);
                        Attach(shard, *ins);
                    }
                } else {
                    auto [it, inserted]{shard.map.try_emplace(op.outpoint, op.key_hash)};
                    Entry& entry{it->second};
                    const State from{StateOf(entry)};
                    if (inserted) {
                        Resized(shard);
                        entry.coin = std::move(op.coin);
                        Transit(entry, from, op.possible_overwrite ? Event::ConfirmedWrite : Event::ConfirmedNew, op.height);
                        AddUsage(shard, entry.coin.DynamicMemoryUsage());
                        Attach(shard, *it);
                    } else {
                        Assert(op.possible_overwrite || entry.coin.IsSpent());
                        Detach(shard, *it);
                        SubUsage(shard, entry.coin.DynamicMemoryUsage());
                        entry.coin = std::move(op.coin);
                        Transit(entry, from, Event::ConfirmedWrite, op.height);
                        AddUsage(shard, entry.coin.DynamicMemoryUsage());
                        Attach(shard, *it);
                    }
                }
            }
        }};

        const size_t helpers{pool && active_idx.size() >= COMMIT_OPS_PER_HELPER
                                 ? std::min(pool->WorkersCount(), active_idx.size() / COMMIT_OPS_PER_HELPER)
                                 : 0};
        if (helpers > 0) {
            VisitShardsShared(shard_of, *pool, helpers, apply_shard);
        } else {
            VisitShards(/*start_hint=*/0, shard_of, apply_shard);
        }
    }

    m_tier_block_hash = best_hash;
    m_change_height = best_height;
    m_tier_height = best_height;
    if (publish_height) m_confirmed_height.store(best_height, std::memory_order_release);
}

// ---------------------------------------------------------------------------------------------------------
// Sizing, eviction and write-out runs

unsigned int CoinsStore::GetCacheSize() const
{
    return static_cast<unsigned int>(SumTotals(&Totals::entries));
}

size_t CoinsStore::GetDirtyCount() const noexcept
{
    return SumTotals(&Totals::dirty);
}

size_t CoinsStore::DynamicMemoryUsage() const
{
    return SumTotals(&Totals::bytes) + RegionAheadBytes();
}

int CoinsStore::BoundForBytes(size_t bytes, int tip_height) const
{
    std::vector<int64_t> merged;
    for (const Shard& shard : m_shards) {
        LOCK(shard.mutex);
        if (merged.size() < shard.dirty_spans.size()) merged.resize(shard.dirty_spans.size());
        for (size_t span{0}; span < shard.dirty_spans.size(); ++span) merged[span] += shard.dirty_spans[span].bytes;
    }
    int64_t freed{0};
    for (size_t span{0}; span < merged.size(); ++span) {
        if (freed + merged[span] < static_cast<int64_t>(bytes)) {
            freed += merged[span];
            continue;
        }
        std::map<int32_t, int64_t> by_height;
        for (const Shard& shard : m_shards) {
            LOCK(shard.mutex);
            if (span >= shard.dirty_spans.size()) continue;
            for (const Node* node{shard.dirty_spans[span].head}; node; node = node->second.next) {
                by_height[node->second.height] += DirtyBytes(node->second);
            }
        }
        for (const auto& [h, b] : by_height) {
            freed += b;
            if (freed >= static_cast<int64_t>(bytes)) return std::min<int>(h, tip_height);
        }
    }
    return tip_height;
}

size_t CoinsStore::EvictClean(size_t target_bytes)
{
    if (DynamicMemoryUsage() <= target_bytes) return 0;
    size_t freed{0};
    // Cap clock passes so concurrent hot reads cannot spin EvictClean indefinitely.
    for (int pass{0}; pass < 2 && DynamicMemoryUsage() > target_bytes; ++pass) {
        bool progress{false};
        for (Shard& shard : m_shards) {
            if (DynamicMemoryUsage() <= target_bytes) return freed;
            LOCK(shard.mutex);
            // Walk up to the current clean entries once per pass.
            Node* stop_at{shard.clean_young};
            while (shard.clean_old && DynamicMemoryUsage() > target_bytes) {
                Node* node{shard.clean_old};
                const bool reached_stop{node == stop_at};
                if (node->second.referenced) {
                    node->second.referenced = false;
                    Detach(shard, *node);
                    Attach(shard, *node);
                    progress = true;
                    if (reached_stop) break;
                    continue;
                }
                EraseEntry(shard, *node);
                ++freed;
                progress = true;
                if (reached_stop) break;
            }
        }
        if (!progress) break;
    }
    return freed;
}

std::unique_ptr<CoinsStore::WriteOutRun> CoinsStore::BeginWriteOut(int bound_height, int head_height, const uint256& new_head, bool final)
{
    Assert(!m_run_in_flight);
    // Promote m_fell_below_written before checking early exit so disconnects below written_height always
    // trigger a final run (and never drop restored coins).
    final = final || m_fell_below_written;
    if (!final) {
        bool any{!m_restored.empty()};
        if (!any) {
            for (const Shard& shard : m_shards) {
                LOCK(shard.mutex);
                for (const Bucket& bucket : shard.dirty_spans) {
                    if (!bucket.head) continue;
                    for (const Node* node{bucket.head}; node && !any; node = node->second.next) {
                        any = node->second.height <= bound_height;
                    }
                    break;
                }
                if (any) break;
            }
        }
        if (!any && bound_height <= m_written_height) return nullptr;
    }
    auto run{std::make_unique<WriteOutRun>()};
    run->restored = TakeRestored();
    run->new_head = new_head;
    run->old_head = m_written_head;
    run->bound_height = final ? head_height : bound_height;
    run->head_height = head_height;
    run->final = final;
    m_inflight_settled_height = run->bound_height;
    m_run_in_flight = true;
    m_write_out_epoch.fetch_add(1, std::memory_order_release);
    if (final) m_fell_below_written = false;
    return run;
}

static uint64_t BatchBytes(const CoinWrite& write)
{
    return write.coin ? 45 + write.coin->out.scriptPubKey.size() : 38;
}

struct WriteSorter {
    std::vector<std::pair<uint64_t, uint32_t>> order;
    std::vector<CoinWrite> sorted;
};

static void SortWrites(std::vector<CoinWrite>& writes, size_t from, WriteSorter& sorter)
{
    const size_t count{writes.size() - from};
    if (count < 2) return;
    sorter.order.clear();
    sorter.order.reserve(count);
    for (size_t i{0}; i < count; ++i) {
        sorter.order.emplace_back(ReadBE64(writes[from + i].outpoint.hash.ToUint256().data()), static_cast<uint32_t>(i));
    }
    std::sort(sorter.order.begin(), sorter.order.end(), [&](const auto& a, const auto& b) {
        if (a.first != b.first) return a.first > b.first;
        return writes[from + b.second].outpoint < writes[from + a.second].outpoint;
    });
    sorter.sorted.clear();
    sorter.sorted.reserve(count);
    for (const auto& [prefix, i] : sorter.order) sorter.sorted.push_back(std::move(writes[from + i]));
    std::move(sorter.sorted.begin(), sorter.sorted.end(), writes.begin() + from);
    sorter.sorted.clear();
}

void CoinsStore::CollectChunk(Shard& shard, WriteOutRun& run, uint64_t limit, uint64_t& size, std::vector<CoinWrite>& writes, std::vector<Node*>& nodes) const
{
    AssertLockHeld(shard.mutex);
    for (size_t span{0}; span < shard.dirty_spans.size(); ++span) {
        int oldest_left{std::numeric_limits<int>::max()};
        for (Node* node{shard.dirty_spans[span].head}, *next{nullptr}; node; node = next) {
            next = node->second.next;
            Entry& entry{node->second};
            if (!run.final && entry.height > run.bound_height) {
                oldest_left = std::min<int>(oldest_left, entry.height);
                continue;
            }
            if (size >= limit) return;
            // A confirmed coin created above the bound that overwrote an older owed erase (so entry.height
            // stayed at the erase's height): write the erase now and move the entry to its coin's height so
            // a partial run never writes a future coin into an earlier settled state.
            if (!run.final && !entry.coin.IsSpent() && !entry.fresh && static_cast<int>(entry.coin.nHeight) > run.bound_height) {
                const int created{static_cast<int>(entry.coin.nHeight)};
                writes.push_back({node->first, std::nullopt});
                size += BatchBytes(writes.back());
                ++run.count;
                Detach(shard, *node);
                Transit(entry, Event::RunWritesOwed, created);
                Attach(shard, *node);
                run.oldest_left = std::min<int>(run.oldest_left, created);
                continue;
            }
            writes.push_back({node->first, entry.coin.IsSpent() ? std::nullopt : std::make_optional(entry.coin.ToCoin())});
            nodes.push_back(node);
            size += BatchBytes(writes.back());
            ++run.count;
            run.bytes += static_cast<size_t>(DirtyBytes(entry));
            Transit(entry, Event::RunCollects);
        }
        if (oldest_left != std::numeric_limits<int>::max()) {
            run.oldest_left = std::min<int>(run.oldest_left, oldest_left);
            return;
        }
    }
}

void CoinsStore::SettleNode(Shard& shard, Node& node) const
{
    AssertLockHeld(shard.mutex);
    Entry& entry{node.second};
    if (!entry.collected) return;
    Detach(shard, node);
    Transit(entry, Event::RunLands);
    if (entry.coin.IsSpent()) {
        EraseEntry(shard, node);
        return;
    }
    Attach(shard, node);
}

void CoinsStore::StreamWriteOut(WriteOutRun& run, const std::function<uint256(int)>& block_at, uint64_t batch_bytes)
{
    const uint64_t limit{std::max<uint64_t>(batch_bytes, 1)};
    uint64_t size{0};
    std::vector<CoinWrite> writes;
    std::vector<Node*> nodes;
    std::vector<std::pair<Shard*, size_t>> spans;
    WriteSorter sorter;
    const auto write_chunk{[&] {
        if (writes.empty()) return;
        SortWrites(writes, 0, sorter);
        base->WriteRun(writes, run.new_head, run.old_head, /*final=*/false);
        ++run.chunks;
        for (size_t i{0}; i < spans.size(); ++i) {
            Shard& shard{*spans[i].first};
            const size_t end{i + 1 < spans.size() ? spans[i + 1].second : nodes.size()};
            LOCK(shard.mutex);
            for (size_t n{spans[i].second}; n < end; ++n) SettleNode(shard, *nodes[n]);
            ++shard.settles;
        }
        writes.clear();
        nodes.clear();
        spans.clear();
        size = 0;
    }};
    for (size_t index{m_shards.size()}; index-- > 0;) {
        Shard& shard{m_shards[index]};
        while (true) {
            const size_t before{nodes.size()};
            WITH_LOCK(shard.mutex, CollectChunk(shard, run, limit, size, writes, nodes));
            if (nodes.size() > before) spans.emplace_back(&shard, before);
            if (size < limit) break;
            write_chunk();
        }
    }
    write_chunk();
    run.settled_height = std::min(run.head_height, run.oldest_left == std::numeric_limits<int>::max() ? run.head_height : run.oldest_left - 1);
    Assume(run.final || run.settled_height >= run.bound_height);
    if (run.final) {
        base->WriteRun({}, run.new_head, run.old_head, /*final=*/true);
        return;
    }
    run.settled_head = block_at(run.settled_height);
    base->WriteRun({}, run.new_head, run.settled_head, /*final=*/false);
}

void CoinsStore::CommitWriteOut(const WriteOutRun& run)
{
    Assert(m_run_in_flight);
    Assert(run.final || !run.settled_head.IsNull());
    m_written_head = run.final ? run.new_head : run.settled_head;
    m_written_height = run.settled_height;
    m_run_in_flight = false;
    m_write_out_epoch.fetch_add(1, std::memory_order_release);
}

void CoinsStore::AbandonWriteOut()
{
    Assert(m_run_in_flight);
    m_flush_failed = true;
    m_run_in_flight = false;
}

static constexpr uint64_t MAX_WIPE_WORKERS{4};

struct CoinsStore::Wipe {
    uint256 best;
    uint256 old_head;
    uint64_t limit;
    std::atomic<bool> failed{false};
};

struct CoinsStore::WipeWorker {
    size_t begin{0};
    size_t end{0};
    std::function<void()> after_first_batch;
    std::vector<CoinWrite> writes;
    WriteSorter sorter;
    uint64_t size{0};
    std::vector<Shard*> held;
    std::exception_ptr error;
};

void CoinsStore::WriteWipeBatch(const Wipe& wipe, WipeWorker& worker)
{
    if (!worker.writes.empty()) {
        base->WriteRun(worker.writes, wipe.best, wipe.old_head, /*final=*/false);
        worker.writes.clear();
        worker.size = 0;
        if (worker.after_first_batch) std::exchange(worker.after_first_batch, nullptr)();
    }
    for (Shard* shard : worker.held) shard->mutex.unlock();
    worker.held.clear();
}

uint64_t CoinsStore::WipeShard(Shard& shard, WipeWorker& worker)
{
    AssertLockHeld(shard.mutex);
    FreeDirtySpans(shard);
    MyTotals().dirty.fetch_sub(static_cast<int64_t>(shard.dirty_count), std::memory_order_relaxed);
    shard.dirty_count = 0;
    shard.clean_young = nullptr;
    shard.clean_old = nullptr;
    uint64_t added{0};
    const size_t from{worker.writes.size()};
    const auto add{[&](CoinWrite&& write) {
        const uint64_t bytes{BatchBytes(write)};
        added += bytes;
        worker.size += bytes;
        worker.writes.push_back(std::move(write));
    }};
    for (auto it{shard.map.begin()}; it != shard.map.end();) {
        Entry& entry{it->second};
        entry.list = LIST_NONE;
        Transit(entry, Event::Flush);
        const size_t usage{entry.coin.DynamicMemoryUsage()};
        if (entry.dirty) {
            add({it->first, entry.coin.IsSpent() ? std::nullopt : std::make_optional(entry.coin.ToCoin())});
        }
        SubUsage(shard, usage);
        it = shard.map.erase(it);
    }
    SortWrites(worker.writes, from, worker.sorter);
    shard.map.rehash(0);
    Resized(shard);
    ++shard.settles;
    return added;
}

static constexpr size_t MAX_BATCH_SHARDS{32};

void CoinsStore::RunWipeWorker(Wipe& wipe, WipeWorker& worker)
{
    Shard* walked{nullptr};
    try {
        for (size_t index{worker.end}; index-- > worker.begin && !wipe.failed.load(std::memory_order_relaxed);) {
            if (worker.held.size() >= MAX_BATCH_SHARDS) WriteWipeBatch(wipe, worker);
            Shard& shard{m_shards[index]};
            EnterCritical("shard.mutex", __FILE__, __LINE__, static_cast<ShardMutex::unique_lock::mutex_type*>(&shard.mutex));
            shard.mutex.lock();
            walked = &shard;
            const uint64_t added{WipeShard(shard, worker)};
            walked = nullptr;
            LeaveCritical();
            worker.held.push_back(&shard);
            if (worker.size >= wipe.limit || worker.size + added > wipe.limit) WriteWipeBatch(wipe, worker);
        }
        WriteWipeBatch(wipe, worker);
    } catch (const std::runtime_error&) {
        worker.error = std::current_exception();
        wipe.failed.store(true, std::memory_order_relaxed);
        if (walked) {
            LeaveCritical();
            walked->mutex.unlock();
        }
        for (Shard* shard : worker.held) shard->mutex.unlock();
        worker.held.clear();
    }
}

void CoinsStore::Flush(bool /*reallocate_cache*/)
{
    const uint256 best{GetBestBlock()};
    const int head_height{m_tier_height >= 0 ? m_tier_height : m_change_height};
    if (m_flush_failed) throw std::runtime_error{"the coins are not written: an earlier flush failed"};
    Assume(m_restored.empty());
    Assert(!m_run_in_flight);
    m_run_in_flight = true;
    Wipe wipe{.best = best, .old_head = m_written_head, .limit = std::max<uint64_t>(m_batch_bytes, 1)};
    const uint64_t cores{std::max(1U, std::thread::hardware_concurrency())};
    const uint64_t wanted{m_base_concurrent ? GetDirtyCount() * uint64_t{38} / wipe.limit / 2 : 1};
    std::vector<WipeWorker> workers(std::clamp<uint64_t>(wanted, 1, std::min<uint64_t>({MAX_WIPE_WORKERS, cores, m_shards.size()})));
    for (size_t i{0}; i < workers.size(); ++i) {
        workers[i].begin = m_shards.size() * i / workers.size();
        workers[i].end = m_shards.size() * (i + 1) / workers.size();
    }
    [&]() noexcept {
        struct Helpers {
            Mutex mutex;
            std::condition_variable cv;
            std::vector<bool> claimed GUARDED_BY(mutex);
            size_t running GUARDED_BY(mutex){0};
        };
        const auto helpers{std::make_shared<Helpers>()};
        WITH_LOCK(helpers->mutex, helpers->claimed.assign(workers.size(), false));
        WipeWorker& first{workers.back()};
        if (workers.size() > 1 && m_flush_helpers) {
            first.after_first_batch = [&] {
                for (size_t i{0}; i + 1 < workers.size(); ++i) {
                    auto task{[this, helpers, i, &wipe, &workers]() noexcept {
                        {
                            LOCK(helpers->mutex);
                            if (helpers->claimed[i]) return;
                            helpers->claimed[i] = true;
                            ++helpers->running;
                        }
                        WAITED_FOR_UNDER_LOCK("a flush helper");
                        RunWipeWorker(wipe, workers[i]);
                        LOCK(helpers->mutex);
                        --helpers->running;
                        helpers->cv.notify_all();
                    }};
                    if (!m_flush_helpers->Submit(std::move(task), /*front=*/true)) break;
                }
            };
        }
        RunWipeWorker(wipe, first);
        for (size_t i{workers.size() - 1}; i-- > 0;) {
            {
                LOCK(helpers->mutex);
                if (helpers->claimed[i]) continue;
                helpers->claimed[i] = true;
            }
            RunWipeWorker(wipe, workers[i]);
        }
        WAIT_LOCK(helpers->mutex, lock);
        helpers->cv.wait(lock, [&]() EXCLUSIVE_LOCKS_REQUIRED(helpers->mutex) { return helpers->running == 0; });
    }();
    for (const WipeWorker& worker : workers) {
        if (!worker.error) continue;
        m_flush_failed = true;
        std::rethrow_exception(worker.error);
    }
    try {
        base->WriteRun({}, best, wipe.old_head, /*final=*/true);
    } catch (const std::runtime_error&) {
        m_flush_failed = true;
        throw;
    }
    m_written_head = best;
    m_written_height = head_height;
    m_fell_below_written = false;
    m_run_in_flight = false;
    m_write_out_epoch.fetch_add(1, std::memory_order_release);
}

void CoinsStore::Sync()
{
    if (m_flush_failed) throw std::runtime_error{"the coins are not written: an earlier flush failed"};
    Assume(m_restored.empty());
    const uint256 best{GetBestBlock()};
    const int head_height{m_tier_height >= 0 ? m_tier_height : m_change_height};
    auto run{BeginWriteOut(/*bound_height=*/head_height, head_height, /*new_head=*/best, /*final=*/true)};
    try {
        StreamWriteOut(*run, /*block_at=*/{}, m_batch_bytes);
    } catch (const std::runtime_error&) {
        AbandonWriteOut();
        throw;
    }
    CommitWriteOut(*run);
}

void CoinsStore::Reset() noexcept
{
}

void CoinsStore::SanityCheck() const
{
    size_t total_bytes{0};
    size_t total_entries{0};
    size_t total_dirty{0};
    for (const Shard& shard : m_shards) {
        LOCK(shard.mutex);
        size_t usage{0};
        size_t dirty{0};
        size_t clean_listed{0};
        std::map<size_t, std::pair<size_t, int64_t>> by_span;
        for (const auto& [outpoint, entry] : shard.map) {
            usage += entry.coin.DynamicMemoryUsage();
            if (entry.dirty) {
                ++dirty;
                auto& [count, bytes]{by_span[SpanOf(entry.height)]};
                ++count;
                bytes += DirtyBytes(entry);
            }
            assert(coins_entry::IsStored(StateOf(entry)));
            const List list{entry.dirty ? LIST_DIRTY : LIST_CLEAN};
            assert(entry.list == list);
            if (list == LIST_CLEAN) ++clean_listed;
        }
        assert(dirty == shard.dirty_count);
        assert(shard.map.size() == shard.entries);
        assert(shard.map.bucket_count() == shard.buckets);
        total_bytes += NodeBytes() * shard.map.size() + memusage::MallocUsage(Map::BUCKET_BYTES * shard.map.bucket_count()) + memusage::DynamicUsage(shard.dirty_spans) + usage;
        total_entries += shard.map.size();
        total_dirty += dirty;
        assert(shard.dirty_spans.empty() == (dirty == 0));
        for (size_t span{0}; span < shard.dirty_spans.size(); ++span) {
            const Bucket& bucket{shard.dirty_spans[span]};
            size_t count{0};
            const Node* prev{nullptr};
            for (const Node* node{bucket.head}; node; prev = node, node = node->second.next) {
                assert(node->second.list == LIST_DIRTY && SpanOf(node->second.height) == span);
                assert(node->second.prev == prev);
                ++count;
            }
            const auto it{by_span.find(span)};
            if (it == by_span.end()) {
                assert(count == 0 && bucket.bytes == 0);
            } else {
                assert(it->second.first == count && it->second.second == bucket.bytes);
                by_span.erase(it);
            }
        }
        assert(by_span.empty());
        size_t count{0};
        const Node* prev{nullptr};
        for (const Node* node{shard.clean_young}; node; prev = node, node = node->second.next) {
            assert(node->second.list == LIST_CLEAN);
            assert(node->second.prev == prev);
            ++count;
        }
        assert(shard.clean_old == prev);
        assert(count == clean_listed);
    }
    assert(SumTotals(&Totals::bytes) == total_bytes);
    assert(GetCacheSize() == total_entries);
    assert(SumTotals(&Totals::dirty) == total_dirty);
    assert(GetDirtyCount() == total_dirty);
}

// ---------------------------------------------------------------------------------------------------------
// BlockApplyView

BlockApplyView::BlockApplyView(CoinsStore& store, util::NotNullSharedPtr<ThreadPool> thread_pool, bool deterministic)
    : CoinsViewOverlay(&store, std::move(thread_pool), deterministic), m_store{store}
{
}

BlockApplyView::~BlockApplyView()
{
    StopFetching();
}

void BlockApplyView::PrepareBlock(int height, const WindowOverlay* overlay)
{
    Assert(!m_in_use);
    m_height = height;
    m_overlay = overlay;
    m_in_use = true;
}

void BlockApplyView::Prefetch(const CBlock& block)
{
    Assert(m_in_use);
    std::vector<COutPoint> outpoints;
    size_t est{0};
    for (size_t i{1}; i < block.vtx.size(); ++i) est += block.vtx[i]->vin.size();
    outpoints.reserve(est);
    for (size_t i{1}; i < block.vtx.size(); ++i) {
        for (const CTxIn& in : block.vtx[i]->vin) outpoints.push_back(in.prevout);
    }
    if (outpoints.empty()) return;

    std::vector<std::pair<COutPoint, Coin>> found;
    if (m_overlay) {
        std::vector<COutPoint> misses;
        misses.reserve(outpoints.size());
        m_overlay->Fetch(m_height, outpoints, found, misses);
        for (auto& [outpoint, coin] : found) {
            const size_t usage{coin.DynamicMemoryUsage()};
            if (cacheCoins.try_emplace(outpoint, std::move(coin)).second) cachedCoinsUsage += usage;
        }
        outpoints = std::move(misses);
    }
    if (!outpoints.empty()) {
        for (auto& [outpoint, coin] : m_store.FetchConfirmed(std::move(outpoints))) {
            const size_t usage{coin.DynamicMemoryUsage()};
            if (cacheCoins.try_emplace(outpoint, std::move(coin)).second) cachedCoinsUsage += usage;
        }
    }
}

bool BlockApplyView::SpendCoin(const COutPoint& outpoint, Coin* moveto)
{
    Coin tmp;
    Coin* const target{moveto ? moveto : &tmp};
    const bool spent{CCoinsViewCache::SpendCoin(outpoint, target)};
    if (spent && !target->IsSpent()) {
        if (auto it{cacheCoins.find(outpoint)}; it != cacheCoins.end()) {
            // CCoinsViewCache::SpendCoin kept the entry in place as a dirty spent tombstone; preserve the
            // spent coin's creation height (Coin::IsSpent() only tests out.IsNull()) for shard-parallel
            // conflict detection without any side table.
            it->second.coin.nHeight = target->nHeight;
        }
    }
    return spent;
}

CoinsStore::BlockChanges BlockApplyView::ExtractChanges(uint64_t validate_epoch, bool must_be_new)
{
    Assert(m_in_use);
    CoinsStore::BlockChanges out;
    out.height = m_height;
    out.validate_epoch = validate_epoch;
    out.must_be_new = must_be_new;
    if (m_dirty_count > 0) {
        const size_t num_shards{m_store.NumShards()};
        std::vector<CoinsStore::CommitOp> unsorted;
        unsorted.reserve(m_dirty_count);
        out.shard_offsets.assign(num_shards + 1, 0);
        for (auto& [outpoint, entry] : cacheCoins) {
            if (!entry.IsDirty()) continue;
            const bool is_spent{entry.coin.IsSpent()};
            const int32_t spent_h{is_spent ? static_cast<int32_t>(entry.coin.nHeight) : -1};
            if (is_spent) entry.coin.nHeight = 0;
            const bool may_overwrite{!entry.IsFresh() && !is_spent && entry.coin.IsCoinBase()};
            const uint32_t s{m_store.ShardOf(outpoint)};
            ++out.shard_offsets[s + 1];
            unsorted.push_back({
                .outpoint = outpoint,
                .coin = std::move(entry.coin),
                .key_hash = CoinsStore::KeyHashOf(outpoint),
                .shard = s,
                .height = m_height,
                .spent_height = spent_h,
                .possible_overwrite = may_overwrite,
                .cancelled = false,
            });
        }
        for (size_t s{0}; s < num_shards; ++s) out.shard_offsets[s + 1] += out.shard_offsets[s];
        std::vector<uint32_t> next(out.shard_offsets.begin(), out.shard_offsets.begin() + num_shards);
        out.ops.resize(unsorted.size());
        for (CoinsStore::CommitOp& op : unsorted) {
            out.ops[next[op.shard]++] = std::move(op);
        }
    }
    cacheCoins.clear();
    cachedCoinsUsage = 0;
    m_dirty_count = 0;
    return out;
}

void BlockApplyView::Confirm(const uint256& block_hash)
{
    if (!Assume(AllInputsConsumed())) {
        LogWarning("Block %s input prevout prefetch queue was not fully consumed; inputs were accessed out of order, so prefetching degraded to serial lookups for this block.", GetBestBlock().ToString());
    }
    StopFetching();
    auto block_changes{ExtractChanges()};
    std::array<CoinsStore::BlockChanges*, 1> batch{&block_changes};
    m_store.ApplyCheckedBlocks(batch, block_hash, m_height, /*pool=*/nullptr, /*publish_height=*/true);
    Reset();
}

void BlockApplyView::Flush(bool /*reallocate_cache*/)
{
    Confirm(GetBestBlock());
}

std::optional<Coin> BlockApplyView::ReadBase(const COutPoint& outpoint) const
{
    if (m_overlay) {
        if (auto coin{m_overlay->Lookup(m_height, outpoint)}) return coin;
    }
    return m_store.PeekCoin(outpoint);
}

void BlockApplyView::Reset() noexcept
{
    StopFetching();
    m_in_use = false;
    m_height = -1;
    m_overlay = nullptr;
    CoinsViewOverlay::Reset();
}
