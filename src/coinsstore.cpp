// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <coinsstore.h>

#include <memusage.h>
#include <util/check.h>

#include <algorithm>
#include <cassert>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {

bool IsCoin(const Coin& coin, const Coin& known)
{
    return coin.nHeight == known.nHeight && coin.fCoinBase == known.fCoinBase;
}

//! Order writes descending by the database key layout (txid prefix, then COutPoint) so disjoint
//! shard chunks emitted from the highest shard down align with LevelDB's SSTable key ranges.
void SortWritesDescending(std::vector<CCoinsView::SortedCoinWrite>& writes)
{
    if (writes.size() < 2) return;
    std::sort(writes.begin(), writes.end(), [](const auto& a, const auto& b) {
        const uint64_t pa{ReadBE64(a.first.hash.ToUint256().data())};
        const uint64_t pb{ReadBE64(b.first.hash.ToUint256().data())};
        if (pa != pb) return pa > pb;
        return b.first < a.first;
    });
}

} // namespace

CoinsStore::CoinsStore(CCoinsView* base_view, size_t shards, bool deterministic)
    : CCoinsViewCache(base_view, deterministic),
      m_deterministic{deterministic},
      m_shards(std::max<size_t>(1, shards))
{
}

CoinsStore::~CoinsStore() = default;

CoinsStore::ShardMap& CoinsStore::EnsureMap(Shard& shard) const
{
    AssertLockHeld(shard.mutex);
    if (!shard.map) {
        shard.resource.emplace(SHARD_POOL_CHUNK_BYTES);
        shard.map.emplace(0, SaltedCoinsCacheHasher{m_deterministic}, std::equal_to<COutPoint>{}, ShardAllocator{&*shard.resource});
    }
    return *shard.map;
}

CoinsStore::Totals& CoinsStore::MyTotals() const
{
    static std::atomic<size_t> next{0};
    static thread_local const size_t stripe{next.fetch_add(1, std::memory_order_relaxed) % TOTALS_STRIPES};
    return m_totals[stripe];
}

size_t CoinsStore::SumTotals(std::atomic<int64_t> Totals::* field) const
{
    int64_t total{0};
    for (const Totals& totals : m_totals) {
        total += (totals.*field).load(std::memory_order_relaxed);
    }
    return static_cast<size_t>(std::max<int64_t>(0, total));
}

void CoinsStore::Resized(Shard& shard) const
{
    AssertLockHeld(shard.mutex);
    const size_t new_entries{shard.map ? shard.map->size() : 0};
    const size_t new_map_bytes{shard.map ? memusage::DynamicUsage(*shard.map) : 0};
    if (new_entries == shard.entries && new_map_bytes == shard.map_bytes) return;
    Totals& totals{MyTotals()};
    totals.entries.fetch_add(static_cast<int64_t>(new_entries) - static_cast<int64_t>(shard.entries), std::memory_order_relaxed);
    totals.bytes.fetch_add(static_cast<int64_t>(new_map_bytes) - static_cast<int64_t>(shard.map_bytes), std::memory_order_relaxed);
    shard.entries = new_entries;
    shard.map_bytes = new_map_bytes;
}

void CoinsStore::AddUsage(Shard& shard, size_t bytes) const
{
    AssertLockHeld(shard.mutex);
    if (bytes == 0) return;
    MyTotals().bytes.fetch_add(static_cast<int64_t>(bytes), std::memory_order_relaxed);
}

void CoinsStore::SubUsage(Shard& shard, size_t bytes) const
{
    AssertLockHeld(shard.mutex);
    if (bytes == 0) return;
    MyTotals().bytes.fetch_sub(static_cast<int64_t>(bytes), std::memory_order_relaxed);
}

void CoinsStore::MarkDirty(Shard& shard, Entry& entry) const
{
    AssertLockHeld(shard.mutex);
    if (entry.dirty) return;
    entry.dirty = true;
    ++shard.dirty_count;
    MyTotals().dirty.fetch_add(1, std::memory_order_relaxed);
}

void CoinsStore::ClearDirty(Shard& shard, Entry& entry) const
{
    AssertLockHeld(shard.mutex);
    if (!entry.dirty) return;
    entry.dirty = false;
    Assert(shard.dirty_count > 0);
    --shard.dirty_count;
    MyTotals().dirty.fetch_sub(1, std::memory_order_relaxed);
}

std::optional<Coin> CoinsStore::Lookup(const COutPoint& outpoint) const
{
    Shard& shard{ShardFor(outpoint)};
    while (true) {
        uint64_t gen;
        {
            LOCK(shard.mutex);
            if (shard.map) {
                if (const auto it{shard.map->find(outpoint)}; it != shard.map->end()) {
                    return it->second.coin.IsSpent() ? std::nullopt : std::make_optional(it->second.coin);
                }
            }
            gen = shard.generation;
        }
        std::optional<Coin> coin{base->GetCoin(outpoint)};
        LOCK(shard.mutex);
        if (shard.generation != gen) continue;
        if (shard.map) {
            if (const auto it{shard.map->find(outpoint)}; it != shard.map->end()) {
                return it->second.coin.IsSpent() ? std::nullopt : std::make_optional(it->second.coin);
            }
        }
        if (!coin) return std::nullopt;
        Assert(!coin->IsSpent());
        auto [it, inserted]{EnsureMap(shard).try_emplace(outpoint)};
        Assert(inserted);
        it->second.coin = *coin;
        AddUsage(shard, it->second.coin.DynamicMemoryUsage());
        Resized(shard);
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
    {
        LOCK(shard.mutex);
        if (shard.map) {
            if (const auto it{shard.map->find(outpoint)}; it != shard.map->end()) {
                return it->second.coin.IsSpent() ? std::nullopt : std::make_optional(it->second.coin);
            }
        }
    }
    return base->PeekCoin(outpoint);
}

bool CoinsStore::HaveCoin(const COutPoint& outpoint) const
{
    return Lookup(outpoint).has_value();
}

bool CoinsStore::HaveCoinInCache(const COutPoint& outpoint) const
{
    Shard& shard{ShardFor(outpoint)};
    LOCK(shard.mutex);
    if (!shard.map) return false;
    const auto it{shard.map->find(outpoint)};
    return it != shard.map->end() && !it->second.coin.IsSpent();
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
    if (m_store_block_hash.IsNull()) {
        m_store_block_hash = base->GetBestBlock();
    }
    return m_store_block_hash;
}

void CoinsStore::SetBestBlock(const uint256& block_hash)
{
    m_store_block_hash = block_hash;
}

void CoinsStore::AddCoin(const COutPoint& outpoint, Coin&& coin, bool possible_overwrite)
{
    assert(!coin.IsSpent());
    if (coin.out.scriptPubKey.IsUnspendable()) return;
    Shard& shard{ShardFor(outpoint)};
    LOCK(shard.mutex);
    auto [it, inserted]{EnsureMap(shard).try_emplace(outpoint)};
    Entry& entry{it->second};
    bool fresh{false};
    if (!possible_overwrite) {
        if (!entry.coin.IsSpent()) {
            if (inserted) Resized(shard);
            throw std::logic_error("Attempted to overwrite an unspent coin (when possible_overwrite is false)");
        }
        // A dirty spent entry records a pending erase from the base DB; re-adding must not mark it FRESH.
        fresh = !entry.dirty;
    }
    if (!inserted) {
        SubUsage(shard, entry.coin.DynamicMemoryUsage());
    }
    entry.coin = std::move(coin);
    entry.fresh = fresh;
    MarkDirty(shard, entry);
    AddUsage(shard, entry.coin.DynamicMemoryUsage());
    if (inserted) Resized(shard);
}

void CoinsStore::EmplaceCoinInternalDANGER(const COutPoint& outpoint, Coin&& coin)
{
    Shard& shard{ShardFor(outpoint)};
    LOCK(shard.mutex);
    auto [it, inserted]{EnsureMap(shard).try_emplace(outpoint)};
    if (!inserted) return;
    Entry& entry{it->second};
    entry.coin = std::move(coin);
    entry.fresh = false;
    MarkDirty(shard, entry);
    AddUsage(shard, entry.coin.DynamicMemoryUsage());
    Resized(shard);
}

void CoinsStore::SpendEntryLocked(Shard& shard, ShardMap::iterator it, Coin* moveto)
{
    AssertLockHeld(shard.mutex);
    Entry& entry{it->second};
    SubUsage(shard, entry.coin.DynamicMemoryUsage());
    if (moveto) *moveto = std::move(entry.coin);
    if (entry.fresh) {
        ClearDirty(shard, entry);
        shard.map->erase(it);
        Resized(shard);
        return;
    }
    entry.coin.Clear();
    MarkDirty(shard, entry);
}

bool CoinsStore::SpendCoin(const COutPoint& outpoint, Coin* moveto)
{
    Shard& shard{ShardFor(outpoint)};
    {
        LOCK(shard.mutex);
        if (shard.map) {
            if (const auto it{shard.map->find(outpoint)}; it != shard.map->end()) {
                SpendEntryLocked(shard, it, moveto);
                return true;
            }
        }
    }
    if (!Lookup(outpoint)) return false;
    LOCK(shard.mutex);
    if (!shard.map) return false;
    const auto it{shard.map->find(outpoint)};
    if (it == shard.map->end()) return false;
    SpendEntryLocked(shard, it, moveto);
    return true;
}

void CoinsStore::Uncache(const COutPoint& outpoint)
{
    Shard& shard{ShardFor(outpoint)};
    LOCK(shard.mutex);
    if (!shard.map) return;
    const auto it{shard.map->find(outpoint)};
    if (it == shard.map->end() || it->second.dirty) return;
    TraceUncache(outpoint, it->second.coin);
    SubUsage(shard, it->second.coin.DynamicMemoryUsage());
    shard.map->erase(it);
    Resized(shard);
}

void CoinsStore::BatchWrite(CoinsViewCacheCursor& cursor, const uint256& block_hash)
{
    for (auto it{cursor.Begin()}; it != cursor.End(); it = cursor.NextAndMaybeErase(*it)) {
        if (!it->second.IsDirty()) continue;
        const COutPoint& outpoint{it->first};
        Shard& shard{ShardFor(outpoint)};
        LOCK(shard.mutex);
        auto [ours, inserted]{EnsureMap(shard).try_emplace(outpoint)};
        Entry& entry{ours->second};
        if (inserted) {
            if (it->second.IsFresh() && it->second.coin.IsSpent()) {
                shard.map->erase(ours);
                Resized(shard);
                continue;
            }
            entry.coin = cursor.WillErase(*it) ? std::move(it->second.coin) : it->second.coin;
            entry.fresh = it->second.IsFresh() && !entry.coin.IsSpent();
            MarkDirty(shard, entry);
            AddUsage(shard, entry.coin.DynamicMemoryUsage());
            Resized(shard);
        } else {
            if (it->second.IsFresh() && !entry.coin.IsSpent()) {
                throw std::logic_error("FRESH flag misapplied to coin that exists in parent cache");
            }
            if (entry.fresh && it->second.coin.IsSpent()) {
                SubUsage(shard, entry.coin.DynamicMemoryUsage());
                ClearDirty(shard, entry);
                shard.map->erase(ours);
                Resized(shard);
                continue;
            }
            SubUsage(shard, entry.coin.DynamicMemoryUsage());
            entry.coin = cursor.WillErase(*it) ? std::move(it->second.coin) : it->second.coin;
            entry.fresh = false;
            MarkDirty(shard, entry);
            AddUsage(shard, entry.coin.DynamicMemoryUsage());
        }
    }
    SetBestBlock(block_hash);
}

template <typename Work>
void CoinsStore::VisitShards(const std::vector<uint32_t>& shard_of, Work work) const
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
    const auto items{[&](size_t s) { return std::span<const uint32_t>{order}.subspan(starts[s], starts[s + 1] - starts[s]); }};
    std::vector<uint32_t> busy;
    for (size_t s{0}; s < shards; ++s) {
        if (starts[s] == starts[s + 1]) continue;
        Shard& shard{m_shards[s]};
        TRY_LOCK(shard.mutex, lock);
        if (!lock) {
            busy.push_back(static_cast<uint32_t>(s));
            continue;
        }
        work(shard, items(s));
    }
    for (const uint32_t b : busy) {
        Shard& shard{m_shards[b]};
        LOCK(shard.mutex);
        work(shard, items(b));
    }
}

uint32_t CoinsStore::ApplyConfirmed(std::vector<Change>&& changes,
                                    ConfirmedReport* report,
                                    const std::function<const Coin*(const COutPoint&)>& base_coin)
{
    std::erase_if(changes, [](const Change& change) {
        return !change.coin->IsSpent() && (change.script ? *change.script : change.coin->out.scriptPubKey).IsUnspendable();
    });
    if (changes.empty()) return 0;

    std::vector<uint32_t> shard_of;
    shard_of.reserve(changes.size());
    for (const Change& change : changes) {
        shard_of.push_back(static_cast<uint32_t>(ShardIndex(*change.outpoint)));
    }

    uint32_t conflicts{0};
    std::vector<const Change*> misses;
    VisitShards(shard_of, [&](Shard& shard, std::span<const uint32_t> items) EXCLUSIVE_LOCKS_REQUIRED(shard.mutex) {
        for (const uint32_t i : items) {
            const Change& change{changes[i]};
            const COutPoint& outpoint{*change.outpoint};
            if (change.coin->IsSpent()) {
                if (!shard.map) {
                    misses.push_back(&change);
                    continue;
                }
                const auto it{shard.map->find(outpoint)};
                if (it == shard.map->end()) {
                    misses.push_back(&change);
                } else if (it->second.coin.IsSpent() || (change.known_coin && !IsCoin(it->second.coin, *change.coin))) {
                    ++conflicts;
                    if (report) report->conflicted.push_back(change.outpoint);
                } else {
                    SpendEntryLocked(shard, it, /*moveto=*/nullptr);
                }
                continue;
            }
            auto [it, inserted]{EnsureMap(shard).try_emplace(outpoint)};
            Entry& entry{it->second};
            const bool over{!inserted && !entry.coin.IsSpent()};
            if (over && !change.possible_overwrite) {
                ++conflicts;
                if (report) report->conflicted.push_back(change.outpoint);
                continue;
            }
            if (over && report) report->overwritten.emplace_back(change.outpoint, entry.coin);
            if (inserted && change.possible_overwrite && report) report->unread.push_back(change.outpoint);
            const bool fresh{!change.possible_overwrite && !entry.dirty};
            if (!inserted) {
                SubUsage(shard, entry.coin.DynamicMemoryUsage());
            }
            entry.coin = std::move(*change.coin);
            if (change.script) {
                entry.coin.out.scriptPubKey = *change.script;
            }
            entry.fresh = fresh;
            MarkDirty(shard, entry);
            AddUsage(shard, entry.coin.DynamicMemoryUsage());
            if (inserted) Resized(shard);
        }
    });

    for (const Change* change : misses) {
        const Coin* const given{base_coin ? base_coin(*change->outpoint) : nullptr};
        const std::optional<Coin> coin{given ? std::optional<Coin>{*given} : Lookup(*change->outpoint)};
        if (!coin || (change->known_coin && !IsCoin(*coin, *change->coin))) {
            ++conflicts;
            if (report) report->conflicted.push_back(change->outpoint);
            continue;
        }
        Shard& shard{ShardFor(*change->outpoint)};
        LOCK(shard.mutex);
        auto [it, inserted]{EnsureMap(shard).try_emplace(*change->outpoint)};
        if (inserted) {
            it->second.coin = *coin;
            AddUsage(shard, it->second.coin.DynamicMemoryUsage());
            Resized(shard);
        }
        if (it->second.coin.IsSpent() || (change->known_coin && !IsCoin(it->second.coin, *change->coin))) {
            ++conflicts;
            if (report) report->conflicted.push_back(change->outpoint);
            continue;
        }
        SpendEntryLocked(shard, it, /*moveto=*/nullptr);
    }

    return conflicts;
}

std::vector<std::pair<COutPoint, Coin>> CoinsStore::Fetch(std::vector<COutPoint>&& outpoints) const
{
    std::vector<uint32_t> shard_of;
    shard_of.reserve(outpoints.size());
    for (const COutPoint& outpoint : outpoints) {
        shard_of.push_back(static_cast<uint32_t>(ShardIndex(outpoint)));
    }
    std::vector<std::pair<COutPoint, Coin>> found;
    found.reserve(outpoints.size());
    VisitShards(shard_of, [&](Shard& shard, std::span<const uint32_t> items) EXCLUSIVE_LOCKS_REQUIRED(shard.mutex) {
        if (!shard.map) return;
        for (const uint32_t i : items) {
            const COutPoint& outpoint{outpoints[i]};
            const auto it{shard.map->find(outpoint)};
            if (it == shard.map->end() || it->second.coin.IsSpent()) continue;
            found.emplace_back(outpoint, it->second.coin);
        }
    });
    return found;
}

void CoinsStore::FetchInto(std::span<const COutPoint> outpoints,
                           std::span<const uint32_t> which,
                           std::vector<Coin>& coins,
                           std::vector<uint8_t>& found) const
{
    found.assign(outpoints.size(), 0);
    if (which.empty()) return;
    std::vector<uint32_t> shard_of;
    shard_of.reserve(which.size());
    for (const uint32_t w : which) {
        shard_of.push_back(static_cast<uint32_t>(ShardIndex(outpoints[w])));
    }
    VisitShards(shard_of, [&](Shard& shard, std::span<const uint32_t> items) EXCLUSIVE_LOCKS_REQUIRED(shard.mutex) {
        if (!shard.map) return;
        for (const uint32_t k : items) {
            const uint32_t w{which[k]};
            const auto it{shard.map->find(outpoints[w])};
            if (it == shard.map->end() || it->second.coin.IsSpent()) continue;
            coins[w] = it->second.coin;
            found[w] = 1;
        }
    });
}

void CoinsStore::MayHave(std::span<const COutPoint> outpoints, std::vector<uint8_t>& may) const
{
    may.assign(outpoints.size(), 0);
    std::vector<uint32_t> shard_of;
    shard_of.reserve(outpoints.size());
    for (const COutPoint& outpoint : outpoints) {
        shard_of.push_back(static_cast<uint32_t>(ShardIndex(outpoint)));
    }
    VisitShards(shard_of, [&](Shard& shard, std::span<const uint32_t> items) EXCLUSIVE_LOCKS_REQUIRED(shard.mutex) {
        if (!shard.map) return;
        for (const uint32_t i : items) {
            if (shard.map->find(outpoints[i]) != shard.map->end()) may[i] = 1;
        }
    });
    for (size_t i{0}; i < outpoints.size(); ++i) {
        if (!may[i] && base->HaveCoin(outpoints[i])) may[i] = 1;
    }
}

void CoinsStore::WriteDirtyToBase(const uint256& best) const
{
    const size_t dirty_count{GetDirtyCount()};
    const size_t total_count{GetCacheSize()};
    base->BatchWriteSorted(
        [&](const std::function<void(std::span<const SortedCoinWrite>)>& emit) {
            std::vector<SortedCoinWrite> writes;
            for (size_t index{m_shards.size()}; index-- > 0;) {
                writes.clear();
                {
                    Shard& shard{m_shards[index]};
                    LOCK(shard.mutex);
                    if (!shard.map || shard.dirty_count == 0) continue;
                    writes.reserve(shard.dirty_count);
                    for (const auto& [outpoint, entry] : *shard.map) {
                        if (!entry.dirty) continue;
                        writes.emplace_back(outpoint, entry.coin.IsSpent() ? std::nullopt : std::make_optional(entry.coin));
                    }
                }
                SortWritesDescending(writes);
                emit(writes);
            }
        },
        best,
        dirty_count,
        total_count);
}

void CoinsStore::Flush(bool reallocate_cache)
{
    const uint256 best{GetBestBlock()};
    if (!best.IsNull()) {
        WriteDirtyToBase(best);
    } else {
        Assert(GetDirtyCount() == 0);
    }
    for (Shard& shard : m_shards) {
        LOCK(shard.mutex);
        if (!shard.map) continue;
        size_t usage{0};
        for (const auto& [_, entry] : *shard.map) {
            usage += entry.coin.DynamicMemoryUsage();
        }
        SubUsage(shard, usage);
        if (shard.dirty_count > 0) {
            MyTotals().dirty.fetch_sub(static_cast<int64_t>(shard.dirty_count), std::memory_order_relaxed);
            shard.dirty_count = 0;
        }
        if (reallocate_cache) {
            shard.map.reset();
            shard.resource.reset();
        } else {
            shard.map->clear();
        }
        Resized(shard);
        ++shard.generation;
    }
}

void CoinsStore::Sync()
{
    const uint256 best{GetBestBlock()};
    if (!best.IsNull()) {
        WriteDirtyToBase(best);
    } else {
        Assert(GetDirtyCount() == 0);
    }
    for (Shard& shard : m_shards) {
        LOCK(shard.mutex);
        if (!shard.map) continue;
        for (auto it{shard.map->begin()}; it != shard.map->end();) {
            Entry& entry{it->second};
            if (!entry.dirty) {
                ++it;
                continue;
            }
            ClearDirty(shard, entry);
            entry.fresh = false;
            if (entry.coin.IsSpent()) {
                assert(entry.coin.DynamicMemoryUsage() == 0);
                it = shard.map->erase(it);
            } else {
                ++it;
            }
        }
        Resized(shard);
        ++shard.generation;
    }
}

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
    return SumTotals(&Totals::bytes);
}

void CoinsStore::SanityCheck() const
{
    size_t total_bytes{0};
    size_t total_entries{0};
    size_t total_dirty{0};
    for (const Shard& shard : m_shards) {
        LOCK(shard.mutex);
        if (!shard.map) {
            assert(shard.dirty_count == 0);
            assert(shard.entries == 0);
            assert(shard.map_bytes == 0);
            continue;
        }
        size_t usage{0};
        size_t dirty{0};
        for (const auto& [outpoint, entry] : *shard.map) {
            usage += entry.coin.DynamicMemoryUsage();
            if (entry.dirty) {
                ++dirty;
            } else {
                assert(!entry.fresh);
                assert(!entry.coin.IsSpent());
            }
            if (entry.fresh) {
                assert(!entry.coin.IsSpent());
            }
        }
        assert(dirty == shard.dirty_count);
        assert(shard.map->size() == shard.entries);
        assert(memusage::DynamicUsage(*shard.map) == shard.map_bytes);
        total_bytes += shard.map_bytes + usage;
        total_entries += shard.entries;
        total_dirty += dirty;
    }
    assert(DynamicMemoryUsage() == total_bytes);
    assert(GetCacheSize() == total_entries);
    assert(GetDirtyCount() == total_dirty);
}

void CoinsStore::Reset() noexcept
{
    // A CoinsStore is the chainstate's persistent tip cache, not a resettable overlay.
}
