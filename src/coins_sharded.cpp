// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <coins_sharded.h>

#include <util/check.h>
#include <util/overflow.h>
#include <util/threadpool.h>
#include <util/trace.h>

#include <algorithm>
#include <cassert>
#include <functional>
#include <initializer_list>
#include <mutex>
#include <stdexcept>

#ifdef ENABLE_TRACING
extern unsigned short utxocache_add_semaphore;
extern unsigned short utxocache_spent_semaphore;
extern unsigned short utxocache_uncache_semaphore;
#endif

namespace {

inline void UpdateAtomicDelta(std::atomic<size_t>& global, size_t& reported, size_t current) noexcept
{
    if (current != reported) {
        if (current > reported) {
            global.fetch_add(current - reported, std::memory_order_relaxed);
        } else {
            global.fetch_sub(reported - current, std::memory_order_relaxed);
        }
        reported = current;
    }
}

void AtomicMin(std::atomic<uint32_t>& target, uint32_t value) noexcept
{
    uint32_t cur = target.load(std::memory_order_relaxed);
    while (value < cur && !target.compare_exchange_weak(cur, value, std::memory_order_relaxed)) {}
}

} // namespace

size_t ShardedCoinsView::Shard::FindPos(const COutPoint& outpoint, uint32_t hash32) const noexcept
{
    if (table.empty()) return NOT_FOUND;
    const size_t mask = table.size() - 1;
    size_t pos = hash32 & mask;
    while (true) {
        const IndexSlot& s = table[pos];
        if (s.tagged_idx == EMPTY_TAG) return NOT_FOUND;
        if (s.hash32 == hash32 && KeyAt(s.tagged_idx) == outpoint) return pos;
        pos = (pos + 1) & mask;
    }
}

void ShardedCoinsView::Shard::EnsureTableCapacity()
{
    // Maintain <= 80% load factor (keeps 1M-slot tables up to ~214M UTXOs without doubling).
    if (!table.empty() && (EntryCount() + 1) * 5 <= table.size() * 4) return;
    const size_t new_cap = table.empty() ? 128 : table.size() * 2;
    const size_t new_mask = new_cap - 1;
    std::vector<IndexSlot, HugePageAllocator<IndexSlot>> new_table(new_cap);
    for (const IndexSlot& slot : table) {
        if (slot.tagged_idx == EMPTY_TAG) continue;
        size_t pos = slot.hash32 & new_mask;
        while (new_table[pos].tagged_idx != EMPTY_TAG) {
            pos = (pos + 1) & new_mask;
        }
        new_table[pos] = slot;
    }
    table.swap(new_table);
}

void ShardedCoinsView::Shard::InsertIndexSlot(uint32_t tagged_idx, uint32_t hash32)
{
    EnsureTableCapacity();
    const size_t mask = table.size() - 1;
    size_t pos = hash32 & mask;
    while (table[pos].tagged_idx != EMPTY_TAG) {
        pos = (pos + 1) & mask;
    }
    table[pos] = IndexSlot{tagged_idx, hash32};
}

void ShardedCoinsView::Shard::EraseIndexPos(size_t pos) noexcept
{
    // Knuth Algorithm R backward-shift deletion: leaves zero hash-table tombstones.
    const size_t mask = table.size() - 1;
    size_t hole = pos;
    size_t cur = (hole + 1) & mask;
    while (table[cur].tagged_idx != EMPTY_TAG) {
        const size_t ideal = table[cur].hash32 & mask;
        if (((hole - ideal) & mask) < ((cur - ideal) & mask)) {
            table[hole] = table[cur];
            hole = cur;
        }
        cur = (cur + 1) & mask;
    }
    table[hole] = IndexSlot{EMPTY_TAG, 0};
}

uint32_t ShardedCoinsView::Shard::AllocLiveSlot(const COutPoint& outpoint, const CTxOut& out, uint32_t height, bool is_coinbase, uint8_t new_flags)
{
    const uint32_t idx = live_storage.Allocate();
    Assert(idx < TOMBSTONE_BIT);
    LiveEntry& entry = live_storage[idx];
    const uint8_t keep_dirty_list = entry.Flags() & FLAG_IN_DIRTY_LIST;
    entry.outpoint = outpoint;
    entry.coin.AssignPreservingFlags(out, height, is_coinbase);
    entry.SetFlags(static_cast<uint8_t>(keep_dirty_list | FLAG_OCCUPIED | (new_flags & (FLAG_DIRTY | FLAG_FRESH))));
    script_dynamic_bytes += entry.coin.DynamicMemoryUsage();
    ++live_count;
    if (entry.Flags() & FLAG_DIRTY) {
        ++dirty_count;
        if (!(entry.Flags() & FLAG_IN_DIRTY_LIST)) {
            entry.OrFlags(FLAG_IN_DIRTY_LIST);
            dirty_live_slots.push_back(idx);
        }
    }
    return idx;
}

uint32_t ShardedCoinsView::Shard::AllocLiveSlot(const COutPoint& outpoint, const Coin& coin, uint8_t new_flags)
{
    return AllocLiveSlot(outpoint, coin.out, coin.nHeight, coin.fCoinBase, new_flags);
}

uint32_t ShardedCoinsView::Shard::AllocLiveSlotStored(const COutPoint& outpoint, StoredCoin&& coin, uint8_t new_flags)
{
    const uint32_t idx = live_storage.Allocate();
    Assert(idx < TOMBSTONE_BIT);
    LiveEntry& entry = live_storage[idx];
    const uint8_t keep_dirty_list = entry.Flags() & FLAG_IN_DIRTY_LIST;
    entry.outpoint = outpoint;
    entry.coin.AssignPreservingFlags(std::move(coin));
    entry.SetFlags(static_cast<uint8_t>(keep_dirty_list | FLAG_OCCUPIED | (new_flags & (FLAG_DIRTY | FLAG_FRESH))));
    script_dynamic_bytes += entry.coin.DynamicMemoryUsage();
    ++live_count;
    if (entry.Flags() & FLAG_DIRTY) {
        ++dirty_count;
        if (!(entry.Flags() & FLAG_IN_DIRTY_LIST)) {
            entry.OrFlags(FLAG_IN_DIRTY_LIST);
            dirty_live_slots.push_back(idx);
        }
    }
    return idx;
}

void ShardedCoinsView::Shard::FreeLiveSlot(uint32_t idx, Coin* moveto)
{
    LiveEntry& entry = live_storage[idx];
    Assert(entry.Flags() & FLAG_OCCUPIED);
    Assume(TrySub(script_dynamic_bytes, entry.coin.DynamicMemoryUsage()));
    if (entry.Flags() & FLAG_DIRTY) {
        Assume(TrySub(dirty_count, size_t{1}));
    }
    Assume(TrySub(live_count, size_t{1}));
    if (moveto) {
        *moveto = entry.coin.ToCoin();
    }
    entry.coin.Clear();
    // Preserve FLAG_IN_DIRTY_LIST so reused slots are never duplicated in dirty_live_slots.
    entry.AndFlags(FLAG_IN_DIRTY_LIST);
    live_storage.Recycle(idx);
}

void ShardedCoinsView::Shard::FreeLiveSlotMoveStored(uint32_t idx, StoredCoin& moveto)
{
    LiveEntry& entry = live_storage[idx];
    Assert(entry.Flags() & FLAG_OCCUPIED);
    Assume(TrySub(script_dynamic_bytes, entry.coin.DynamicMemoryUsage()));
    if (entry.Flags() & FLAG_DIRTY) {
        Assume(TrySub(dirty_count, size_t{1}));
    }
    Assume(TrySub(live_count, size_t{1}));
    moveto.AssignPreservingFlags(std::move(entry.coin));
    // Preserve FLAG_IN_DIRTY_LIST so reused slots are never duplicated in dirty_live_slots.
    entry.AndFlags(FLAG_IN_DIRTY_LIST);
    live_storage.Recycle(idx);
}

uint32_t ShardedCoinsView::Shard::AllocTombSlot(const COutPoint& outpoint, uint8_t new_flags)
{
    const uint32_t idx = tomb_storage.Allocate();
    Assert(idx < TOMBSTONE_BIT);
    TombstoneEntry& entry = tomb_storage[idx];
    const uint8_t keep_dirty_list = entry.flags & FLAG_IN_DIRTY_LIST;
    entry.outpoint = outpoint;
    entry.flags = static_cast<uint8_t>(keep_dirty_list | FLAG_OCCUPIED | (new_flags & FLAG_DIRTY));
    ++tomb_count;
    if (entry.flags & FLAG_DIRTY) {
        ++dirty_count;
        if (!(entry.flags & FLAG_IN_DIRTY_LIST)) {
            entry.flags |= FLAG_IN_DIRTY_LIST;
            dirty_tomb_slots.push_back(idx);
        }
    }
    return idx;
}

void ShardedCoinsView::Shard::FreeTombSlot(uint32_t idx)
{
    TombstoneEntry& entry = tomb_storage[idx];
    Assert(entry.flags & FLAG_OCCUPIED);
    if (entry.flags & FLAG_DIRTY) {
        Assume(TrySub(dirty_count, size_t{1}));
    }
    Assume(TrySub(tomb_count, size_t{1}));
    entry.flags &= FLAG_IN_DIRTY_LIST;
    tomb_storage.Recycle(idx);
}

void ShardedCoinsView::Shard::MarkLiveDirty(uint32_t idx)
{
    LiveEntry& entry = live_storage[idx];
    if (!(entry.Flags() & FLAG_DIRTY)) {
        entry.OrFlags(FLAG_DIRTY);
        ++dirty_count;
    }
    if (!(entry.Flags() & FLAG_IN_DIRTY_LIST)) {
        entry.OrFlags(FLAG_IN_DIRTY_LIST);
        dirty_live_slots.push_back(idx);
    }
}

void ShardedCoinsView::Shard::MarkTombDirty(uint32_t idx)
{
    TombstoneEntry& entry = tomb_storage[idx];
    if (!(entry.flags & FLAG_DIRTY)) {
        entry.flags |= FLAG_DIRTY;
        ++dirty_count;
    }
    if (!(entry.flags & FLAG_IN_DIRTY_LIST)) {
        entry.flags |= FLAG_IN_DIRTY_LIST;
        dirty_tomb_slots.push_back(idx);
    }
}

void ShardedCoinsView::Shard::Clear(bool release_memory)
{
    if (release_memory) {
        std::vector<IndexSlot, HugePageAllocator<IndexSlot>>{}.swap(table);
        std::vector<uint32_t>{}.swap(dirty_live_slots);
        std::vector<uint32_t>{}.swap(dirty_tomb_slots);
    } else {
        std::fill(table.begin(), table.end(), IndexSlot{EMPTY_TAG, 0});
        dirty_live_slots.clear();
        dirty_tomb_slots.clear();
    }
    live_storage.Clear(release_memory);
    tomb_storage.Clear(release_memory);
    live_count = 0;
    tomb_count = 0;
    dirty_count = 0;
    script_dynamic_bytes = 0;
}

ShardedCoinsView::ShardedCoinsView(CCoinsView* in_base, bool deterministic)
    : CCoinsViewBacked(in_base),
      m_hasher{/*deterministic=*/deterministic}
{
}

void ShardedCoinsView::SyncShardAccounting(Shard& shard) const noexcept
{
    UpdateAtomicDelta(m_dynamic_memory_usage, shard.reported_memory_usage, shard.ComputeMemoryUsage());
    UpdateAtomicDelta(m_cached_coins_count, shard.reported_coins_count, shard.EntryCount());
    UpdateAtomicDelta(m_dirty_count, shard.reported_dirty_count, shard.dirty_count);
}

ShardedCoinsView::CacheLookupStatus ShardedCoinsView::PeekCacheStatus(const COutPoint& outpoint, Coin* coin_out) const
{
    const Shard& shard = m_shards[ShardIndex(outpoint)];
    const uint32_t hash32 = HashOutPoint(outpoint);
    std::shared_lock lock{shard.mutex};
    const size_t pos = shard.FindPos(outpoint, hash32);
    if (pos == Shard::NOT_FOUND) return CacheLookupStatus::ABSENT;
    const uint32_t tagged = shard.table[pos].tagged_idx;
    if (tagged & TOMBSTONE_BIT) return CacheLookupStatus::TOMBSTONE;
    if (coin_out) *coin_out = shard.live_storage[tagged].coin.ToCoin();
    return CacheLookupStatus::LIVE;
}

void ShardedCoinsView::LookupBatch(std::span<const COutPoint> outpoints, std::span<BatchLookupResult> results) const
{
    Assert(outpoints.size() == results.size());
    const size_t n = outpoints.size();
    if (n == 0) return;
    if (n == 1) {
        results[0].status = PeekCacheStatus(outpoints[0], &results[0].coin);
        return;
    }

    // Group indices by shard in O(n) via counting sort so each touched shard locks once,
    // and precompute outpoint hashes outside shard locks while keys are hot in L1.
    std::array<uint32_t, NUM_SHARDS> counts{};
    static thread_local std::vector<uint32_t> hashes;
    hashes.resize(n);
    for (size_t i = 0; i < n; ++i) {
        ++counts[ShardIndex(outpoints[i])];
        hashes[i] = HashOutPoint(outpoints[i]);
    }

    std::array<uint32_t, NUM_SHARDS + 1> offsets{};
    for (size_t s = 0; s < NUM_SHARDS; ++s) {
        offsets[s + 1] = offsets[s] + counts[s];
    }

    static thread_local std::vector<uint32_t> order;
    order.resize(n);
    std::array<uint32_t, NUM_SHARDS> write_pos{};
    for (size_t s = 0; s < NUM_SHARDS; ++s) write_pos[s] = offsets[s];
    for (uint32_t i = 0; i < static_cast<uint32_t>(n); ++i) {
        const size_t s = ShardIndex(outpoints[i]);
        order[write_pos[s]++] = i;
    }

    static constexpr uint32_t PREFETCH_TABLE_DIST = 8;
    static constexpr uint32_t PREFETCH_ENTRY_DIST = 4;

    // Stagger shard traversal order across workers to break mutex convoys.
    const size_t start_shard = m_lookup_shard_cursor.fetch_add(1, std::memory_order_relaxed) & (NUM_SHARDS - 1);
    for (size_t k = 0; k < NUM_SHARDS; ++k) {
        const size_t s = (start_shard + k) & (NUM_SHARDS - 1);
        const uint32_t begin = offsets[s];
        const uint32_t end = offsets[s + 1];
        if (begin == end) continue;

        const Shard& shard = m_shards[s];
        std::shared_lock lock{shard.mutex};
        if (shard.table.empty()) {
            for (uint32_t p = begin; p < end; ++p) {
                results[order[p]].status = CacheLookupStatus::ABSENT;
            }
            continue;
        }

        const size_t mask = shard.table.size() - 1;
        const uint32_t prime_end = std::min(end, begin + PREFETCH_TABLE_DIST);
        for (uint32_t p = begin; p < prime_end; ++p) {
            shard.PrefetchTableSlot(hashes[order[p]]);
        }

        for (uint32_t p = begin; p < end; ++p) {
            if (p + PREFETCH_TABLE_DIST < end) {
                shard.PrefetchTableSlot(hashes[order[p + PREFETCH_TABLE_DIST]]);
            }
            if (p + PREFETCH_ENTRY_DIST < end) {
                const uint32_t h_ahead = hashes[order[p + PREFETCH_ENTRY_DIST]];
                const IndexSlot& slot_ahead = shard.table[h_ahead & mask];
                if (slot_ahead.tagged_idx != EMPTY_TAG && slot_ahead.hash32 == h_ahead) {
                    shard.PrefetchEntry(slot_ahead.tagged_idx);
                }
            }

            const uint32_t idx = order[p];
            const COutPoint& op = outpoints[idx];
            const uint32_t hash32 = hashes[idx];
            const size_t pos = shard.FindPos(op, hash32);
            if (pos == Shard::NOT_FOUND) {
                results[idx].status = CacheLookupStatus::ABSENT;
            } else {
                const uint32_t tagged = shard.table[pos].tagged_idx;
                if (tagged & TOMBSTONE_BIT) {
                    results[idx].status = CacheLookupStatus::TOMBSTONE;
                } else {
                    results[idx].status = CacheLookupStatus::LIVE;
                    results[idx].coin = shard.live_storage[tagged].coin.ToCoin();
                }
            }
        }
    }
}

void ShardedCoinsView::PeekCacheStatusBatch(std::span<const COutPoint> outpoints, std::span<CacheLookupStatus> statuses) const
{
    Assert(outpoints.size() == statuses.size());
    const size_t n = outpoints.size();
    if (n == 0) return;
    if (n == 1) {
        statuses[0] = PeekCacheStatus(outpoints[0]);
        return;
    }

    if (n <= 8) {
        struct SmallItem {
            uint8_t shard;
            uint8_t idx;
            uint32_t hash32;
        };
        std::array<SmallItem, 8> items{};
        for (size_t i = 0; i < n; ++i) {
            items[i] = SmallItem{
                static_cast<uint8_t>(ShardIndex(outpoints[i])),
                static_cast<uint8_t>(i),
                HashOutPoint(outpoints[i]),
            };
        }
        std::sort(items.begin(), items.begin() + static_cast<std::ptrdiff_t>(n),
                  [](const SmallItem& a, const SmallItem& b) noexcept { return a.shard < b.shard; });

        size_t i = 0;
        while (i < n) {
            const uint8_t s = items[i].shard;
            const Shard& shard = m_shards[s];
            std::shared_lock lock{shard.mutex};
            if (shard.table.empty()) {
                while (i < n && items[i].shard == s) {
                    statuses[items[i].idx] = CacheLookupStatus::ABSENT;
                    ++i;
                }
                continue;
            }
            while (i < n && items[i].shard == s) {
                const uint8_t idx = items[i].idx;
                const size_t pos = shard.FindPos(outpoints[idx], items[i].hash32);
                if (pos == Shard::NOT_FOUND) {
                    statuses[idx] = CacheLookupStatus::ABSENT;
                } else {
                    const uint32_t tagged = shard.table[pos].tagged_idx;
                    statuses[idx] = (tagged & TOMBSTONE_BIT) ? CacheLookupStatus::TOMBSTONE : CacheLookupStatus::LIVE;
                }
                ++i;
            }
        }
        return;
    }

    std::array<uint32_t, NUM_SHARDS> counts{};
    static thread_local std::vector<uint32_t> hashes;
    hashes.resize(n);
    for (size_t i = 0; i < n; ++i) {
        ++counts[ShardIndex(outpoints[i])];
        hashes[i] = HashOutPoint(outpoints[i]);
    }

    std::array<uint32_t, NUM_SHARDS + 1> offsets{};
    for (size_t s = 0; s < NUM_SHARDS; ++s) {
        offsets[s + 1] = offsets[s] + counts[s];
    }

    static thread_local std::vector<uint32_t> order;
    order.resize(n);
    std::array<uint32_t, NUM_SHARDS> write_pos{};
    for (size_t s = 0; s < NUM_SHARDS; ++s) write_pos[s] = offsets[s];
    for (uint32_t i = 0; i < static_cast<uint32_t>(n); ++i) {
        const size_t s = ShardIndex(outpoints[i]);
        order[write_pos[s]++] = i;
    }

    static constexpr uint32_t PREFETCH_TABLE_DIST = 8;

    const size_t start_shard = m_lookup_shard_cursor.fetch_add(1, std::memory_order_relaxed) & (NUM_SHARDS - 1);
    for (size_t k = 0; k < NUM_SHARDS; ++k) {
        const size_t s = (start_shard + k) & (NUM_SHARDS - 1);
        const uint32_t begin = offsets[s];
        const uint32_t end = offsets[s + 1];
        if (begin == end) continue;

        const Shard& shard = m_shards[s];
        std::shared_lock lock{shard.mutex};
        if (shard.table.empty()) {
            for (uint32_t p = begin; p < end; ++p) {
                statuses[order[p]] = CacheLookupStatus::ABSENT;
            }
            continue;
        }

        const uint32_t prime_end = std::min(end, begin + PREFETCH_TABLE_DIST);
        for (uint32_t p = begin; p < prime_end; ++p) {
            shard.PrefetchTableSlot(hashes[order[p]]);
        }

        for (uint32_t p = begin; p < end; ++p) {
            if (p + PREFETCH_TABLE_DIST < end) {
                shard.PrefetchTableSlot(hashes[order[p + PREFETCH_TABLE_DIST]]);
            }

            const uint32_t idx = order[p];
            const size_t pos = shard.FindPos(outpoints[idx], hashes[idx]);
            if (pos == Shard::NOT_FOUND) {
                statuses[idx] = CacheLookupStatus::ABSENT;
            } else {
                const uint32_t tagged = shard.table[pos].tagged_idx;
                statuses[idx] = (tagged & TOMBSTONE_BIT) ? CacheLookupStatus::TOMBSTONE : CacheLookupStatus::LIVE;
            }
        }
    }
}

std::optional<Coin> ShardedCoinsView::PeekCoin(const COutPoint& outpoint) const
{
    Coin coin;
    switch (PeekCacheStatus(outpoint, &coin)) {
    case CacheLookupStatus::LIVE:
        return coin;
    case CacheLookupStatus::TOMBSTONE:
        return std::nullopt;
    case CacheLookupStatus::ABSENT:
        return base->PeekCoin(outpoint);
    }
    AssertUnreachable();
}

std::optional<Coin> ShardedCoinsView::GetCoin(const COutPoint& outpoint) const
{
    Coin cached_coin;
    switch (PeekCacheStatus(outpoint, &cached_coin)) {
    case CacheLookupStatus::LIVE:
        return cached_coin;
    case CacheLookupStatus::TOMBSTONE:
        return std::nullopt;
    case CacheLookupStatus::ABSENT:
        break;
    }

    // Fetch from base outside the shard lock.
    auto fetched = base->GetCoin(outpoint);
    if (!fetched) return std::nullopt;
    Assert(!fetched->IsSpent());

    Shard& shard = m_shards[ShardIndex(outpoint)];
    const uint32_t hash32 = HashOutPoint(outpoint);
    std::unique_lock lock{shard.mutex};
    const size_t pos = shard.FindPos(outpoint, hash32);
    if (pos != Shard::NOT_FOUND) {
        const uint32_t tagged = shard.table[pos].tagged_idx;
        if (tagged & TOMBSTONE_BIT) return std::nullopt;
        return shard.live_storage[tagged].coin.ToCoin();
    }

    const uint32_t idx = shard.AllocLiveSlot(outpoint, *fetched, /*new_flags=*/0);
    shard.InsertIndexSlot(idx, hash32);
    SyncShardAccounting(shard);
    return fetched;
}

bool ShardedCoinsView::HaveCoin(const COutPoint& outpoint) const
{
    return GetCoin(outpoint).has_value();
}

bool ShardedCoinsView::HaveCoinInCache(const COutPoint& outpoint) const
{
    return PeekCacheStatus(outpoint) == CacheLookupStatus::LIVE;
}

uint256 ShardedCoinsView::GetBestBlock() const
{
    LOCK(m_best_block_mutex);
    if (m_block_hash.IsNull()) {
        m_block_hash = base->GetBestBlock();
    }
    return m_block_hash;
}

void ShardedCoinsView::SetBestBlock(const uint256& in_block_hash)
{
    LOCK(m_best_block_mutex);
    m_block_hash = in_block_hash;
}

void ShardedCoinsView::AddCoin(const COutPoint& outpoint, Coin&& coin, bool possible_overwrite)
{
    assert(!coin.IsSpent());
    if (coin.out.scriptPubKey.IsUnspendable()) return;

    Shard& shard = m_shards[ShardIndex(outpoint)];
    const uint32_t hash32 = HashOutPoint(outpoint);
    std::unique_lock lock{shard.mutex};
    const size_t pos = shard.FindPos(outpoint, hash32);

    [[maybe_unused]] const uint32_t coin_height = coin.nHeight;
    [[maybe_unused]] const int64_t coin_value = coin.out.nValue;
    [[maybe_unused]] const bool coin_coinbase = coin.IsCoinBase();

    if (pos == Shard::NOT_FOUND) {
        const uint8_t flags = static_cast<uint8_t>(FLAG_DIRTY | (!possible_overwrite ? FLAG_FRESH : 0));
        const uint32_t idx = shard.AllocLiveSlot(outpoint, coin, flags);
        shard.InsertIndexSlot(idx, hash32);
    } else {
        const uint32_t tagged = shard.table[pos].tagged_idx;
        if (tagged & TOMBSTONE_BIT) {
            // Re-adding a coin that currently has a dirty tombstone: cannot mark FRESH.
            const uint32_t old_tomb = tagged & INDEX_MASK;
            const uint32_t new_live = shard.AllocLiveSlot(outpoint, coin, FLAG_DIRTY);
            shard.table[pos].tagged_idx = new_live;
            shard.FreeTombSlot(old_tomb);
        } else {
            if (!possible_overwrite) {
                throw std::logic_error("Attempted to overwrite an unspent coin (when possible_overwrite is false)");
            }
            LiveEntry& entry = shard.live_storage[tagged];
            Assume(TrySub(shard.script_dynamic_bytes, entry.coin.DynamicMemoryUsage()));
            entry.coin.AssignPreservingFlags(coin);
            shard.script_dynamic_bytes += entry.coin.DynamicMemoryUsage();
            // possible_overwrite == true: must not mark FRESH.
            entry.AndFlags(static_cast<uint8_t>(~FLAG_FRESH));
            shard.MarkLiveDirty(tagged);
        }
    }

    SyncShardAccounting(shard);
    TRACEPOINT(utxocache, add,
        outpoint.hash.data(),
        static_cast<uint32_t>(outpoint.n),
        coin_height,
        coin_value,
        coin_coinbase);
}

void ShardedCoinsView::EmplaceCoinInternalDANGER(const COutPoint& outpoint, Coin&& coin)
{
    Assert(!coin.IsSpent());
    Shard& shard = m_shards[ShardIndex(outpoint)];
    const uint32_t hash32 = HashOutPoint(outpoint);
    std::unique_lock lock{shard.mutex};
    if (shard.FindPos(outpoint, hash32) != Shard::NOT_FOUND) return;
    const uint32_t idx = shard.AllocLiveSlot(outpoint, coin, FLAG_DIRTY);
    shard.InsertIndexSlot(idx, hash32);
    SyncShardAccounting(shard);
}

bool ShardedCoinsView::SpendCoin(const COutPoint& outpoint, Coin* moveto)
{
    Shard& shard = m_shards[ShardIndex(outpoint)];
    const uint32_t hash32 = HashOutPoint(outpoint);

    {
        std::unique_lock lock{shard.mutex};
        const size_t pos = shard.FindPos(outpoint, hash32);
        if (pos != Shard::NOT_FOUND) {
            const uint32_t tagged = shard.table[pos].tagged_idx;
            if (tagged & TOMBSTONE_BIT) return false;

            LiveEntry& entry = shard.live_storage[tagged];
            const bool was_fresh = (entry.Flags() & FLAG_FRESH) != 0;
            TRACEPOINT(utxocache, spent,
                outpoint.hash.data(),
                static_cast<uint32_t>(outpoint.n),
                static_cast<uint32_t>(entry.coin.Height()),
                static_cast<int64_t>(entry.coin.Amount()),
                static_cast<bool>(entry.coin.IsCoinBase()));
            if (was_fresh) {
                shard.EraseIndexPos(pos);
                shard.FreeLiveSlot(tagged, moveto);
            } else {
                const uint32_t tomb_idx = shard.AllocTombSlot(outpoint, FLAG_DIRTY);
                shard.table[pos].tagged_idx = tomb_idx | TOMBSTONE_BIT;
                shard.FreeLiveSlot(tagged, moveto);
            }
            SyncShardAccounting(shard);
            return true;
        }
    }

    // Cache miss: query backing store outside the shard lock.
    auto fetched = base->GetCoin(outpoint);
    if (!fetched) return false;
    Assert(!fetched->IsSpent());

    std::unique_lock lock{shard.mutex};
    const size_t pos = shard.FindPos(outpoint, hash32);
    if (pos != Shard::NOT_FOUND) {
        const uint32_t tagged = shard.table[pos].tagged_idx;
        if (tagged & TOMBSTONE_BIT) return false;
        LiveEntry& entry = shard.live_storage[tagged];
        const bool was_fresh = (entry.Flags() & FLAG_FRESH) != 0;
        TRACEPOINT(utxocache, spent,
            outpoint.hash.data(),
            static_cast<uint32_t>(outpoint.n),
            static_cast<uint32_t>(entry.coin.Height()),
            static_cast<int64_t>(entry.coin.Amount()),
            static_cast<bool>(entry.coin.IsCoinBase()));
        if (was_fresh) {
            shard.EraseIndexPos(pos);
            shard.FreeLiveSlot(tagged, moveto);
        } else {
            const uint32_t tomb_idx = shard.AllocTombSlot(outpoint, FLAG_DIRTY);
            shard.table[pos].tagged_idx = tomb_idx | TOMBSTONE_BIT;
            shard.FreeLiveSlot(tagged, moveto);
        }
        SyncShardAccounting(shard);
        return true;
    }

    TRACEPOINT(utxocache, spent,
        outpoint.hash.data(),
        static_cast<uint32_t>(outpoint.n),
        static_cast<uint32_t>(fetched->nHeight),
        static_cast<int64_t>(fetched->out.nValue),
        static_cast<bool>(fetched->IsCoinBase()));
    if (moveto) {
        *moveto = std::move(*fetched);
    }
    const uint32_t tomb_idx = shard.AllocTombSlot(outpoint, FLAG_DIRTY);
    shard.InsertIndexSlot(tomb_idx | TOMBSTONE_BIT, hash32);
    SyncShardAccounting(shard);
    return true;
}

void ShardedCoinsView::Uncache(const COutPoint& outpoint)
{
    Shard& shard = m_shards[ShardIndex(outpoint)];
    const uint32_t hash32 = HashOutPoint(outpoint);
    std::unique_lock lock{shard.mutex};
    const size_t pos = shard.FindPos(outpoint, hash32);
    if (pos == Shard::NOT_FOUND) return;
    const uint32_t tagged = shard.table[pos].tagged_idx;
    if (tagged & TOMBSTONE_BIT) return;
    LiveEntry& entry = shard.live_storage[tagged];
    if (entry.Flags() & FLAG_DIRTY) return;

    TRACEPOINT(utxocache, uncache,
        outpoint.hash.data(),
        static_cast<uint32_t>(outpoint.n),
        static_cast<uint32_t>(entry.coin.Height()),
        static_cast<int64_t>(entry.coin.Amount()),
        static_cast<bool>(entry.coin.IsCoinBase()));
    shard.EraseIndexPos(pos);
    shard.FreeLiveSlot(tagged);
    SyncShardAccounting(shard);
    m_flush_epoch.fetch_add(1, std::memory_order_release);
}

void ShardedCoinsView::BatchWrite(CoinsViewCacheCursor& cursor, const uint256& in_block_hash)
{
    struct OverlayOp {
        COutPoint outpoint;
        Coin coin;
        bool is_fresh;
        bool is_spent;
    };

    std::vector<OverlayOp> ops;
    ops.reserve(cursor.GetDirtyCount());

    for (auto it{cursor.Begin()}; it != cursor.End(); it = cursor.NextAndMaybeErase(*it)) {
        if (!it->second.IsDirty()) continue;
        OverlayOp op;
        op.outpoint = it->first;
        op.is_fresh = it->second.IsFresh();
        op.is_spent = it->second.coin.IsSpent();
        if (!op.is_spent) {
            if (cursor.WillErase(*it)) {
                op.coin = std::move(it->second.coin);
            } else {
                op.coin = it->second.coin;
            }
        }
        ops.emplace_back(std::move(op));
    }

    if (!ops.empty()) {
        // Group overlay ops by shard so each touched shard locks once,
        // and precompute outpoint hashes outside shard locks.
        std::array<uint32_t, NUM_SHARDS> counts{};
        std::vector<uint32_t> hashes(ops.size());
        for (size_t i = 0; i < ops.size(); ++i) {
            ++counts[ShardIndex(ops[i].outpoint)];
            hashes[i] = HashOutPoint(ops[i].outpoint);
        }
        std::array<uint32_t, NUM_SHARDS + 1> offsets{};
        for (size_t s = 0; s < NUM_SHARDS; ++s) {
            offsets[s + 1] = offsets[s] + counts[s];
        }
        std::vector<uint32_t> order(ops.size());
        std::array<uint32_t, NUM_SHARDS> write_pos{};
        for (size_t s = 0; s < NUM_SHARDS; ++s) write_pos[s] = offsets[s];
        for (uint32_t i = 0; i < static_cast<uint32_t>(ops.size()); ++i) {
            const size_t s = ShardIndex(ops[i].outpoint);
            order[write_pos[s]++] = i;
        }

        for (size_t s = 0; s < NUM_SHARDS; ++s) {
            const uint32_t begin = offsets[s];
            const uint32_t end = offsets[s + 1];
            if (begin == end) continue;

            Shard& shard = m_shards[s];
            std::unique_lock lock{shard.mutex};
            struct AccountingGuard {
                const ShardedCoinsView& view;
                Shard& shard;
                ~AccountingGuard() noexcept { view.SyncShardAccounting(shard); }
            } accounting_guard{*this, shard};

            for (uint32_t p = begin; p < end; ++p) {
                if (p + 4 < end) {
                    shard.PrefetchTableSlot(hashes[order[p + 4]]);
                }
                const uint32_t op_idx = order[p];
                OverlayOp& op = ops[op_idx];
                const uint32_t hash32 = hashes[op_idx];
                const size_t pos = shard.FindPos(op.outpoint, hash32);

                if (pos == Shard::NOT_FOUND) {
                    if (op.is_fresh && op.is_spent) {
                        continue;
                    }
                    if (op.is_spent) {
                        const uint32_t tomb_idx = shard.AllocTombSlot(op.outpoint, FLAG_DIRTY);
                        shard.InsertIndexSlot(tomb_idx | TOMBSTONE_BIT, hash32);
                    } else {
                        const uint8_t flags = static_cast<uint8_t>(FLAG_DIRTY | (op.is_fresh ? FLAG_FRESH : 0));
                        const uint32_t live_idx = shard.AllocLiveSlot(op.outpoint, op.coin, flags);
                        shard.InsertIndexSlot(live_idx, hash32);
                    }
                } else {
                    const uint32_t tagged = shard.table[pos].tagged_idx;
                    const bool parent_is_tomb = (tagged & TOMBSTONE_BIT) != 0;

                    if (op.is_fresh && !parent_is_tomb) {
                        throw std::logic_error("FRESH flag misapplied to coin that exists in parent cache");
                    }

                    if (!parent_is_tomb && (shard.live_storage[tagged].Flags() & FLAG_FRESH) && op.is_spent) {
                        // Grandparent does not have this entry and child spent it: remove entirely.
                        shard.EraseIndexPos(pos);
                        shard.FreeLiveSlot(tagged);
                    } else if (op.is_spent) {
                        if (!parent_is_tomb) {
                            const uint32_t tomb_idx = shard.AllocTombSlot(op.outpoint, FLAG_DIRTY);
                            shard.table[pos].tagged_idx = tomb_idx | TOMBSTONE_BIT;
                            shard.FreeLiveSlot(tagged);
                        } else {
                            shard.MarkTombDirty(tagged & INDEX_MASK);
                        }
                    } else {
                        if (parent_is_tomb) {
                            // Do not mark FRESH: parent had an unflushed tombstone.
                            const uint32_t old_tomb = tagged & INDEX_MASK;
                            const uint32_t live_idx = shard.AllocLiveSlot(op.outpoint, op.coin, FLAG_DIRTY);
                            shard.table[pos].tagged_idx = live_idx;
                            shard.FreeTombSlot(old_tomb);
                        } else {
                            LiveEntry& entry = shard.live_storage[tagged];
                            Assume(TrySub(shard.script_dynamic_bytes, entry.coin.DynamicMemoryUsage()));
                            entry.coin.AssignPreservingFlags(op.coin);
                            shard.script_dynamic_bytes += entry.coin.DynamicMemoryUsage();
                            shard.MarkLiveDirty(tagged);
                        }
                    }
                }
            }
        }
    }

    SetBestBlock(in_block_hash);
}

void ShardedCoinsView::Flush(bool reallocate_cache, ThreadPool* pool)
{
    const uint256 block_hash = GetBestBlock();

    // Collect dirty tagged indices per shard (4 bytes per dirty entry, zero Coin copies).
    std::array<std::vector<uint32_t>, NUM_SHARDS> shard_dirty;
    std::vector<size_t> non_empty_shards;
    non_empty_shards.reserve(NUM_SHARDS);
    size_t total_dirty = 0;
    size_t total_entries = 0;

    for (size_t s = 0; s < NUM_SHARDS; ++s) {
        const Shard& shard = m_shards[s];
        std::shared_lock lock{shard.mutex};
        total_entries += shard.EntryCount();
        if (shard.dirty_count == 0) continue;

        auto& list = shard_dirty[s];
        list.reserve(shard.dirty_count);
        for (uint32_t idx : shard.dirty_live_slots) {
            const LiveEntry& e = shard.live_storage[idx];
            if ((e.Flags() & FLAG_OCCUPIED) && (e.Flags() & FLAG_DIRTY)) {
                list.push_back(idx);
            }
        }
        for (uint32_t idx : shard.dirty_tomb_slots) {
            const TombstoneEntry& e = shard.tomb_storage[idx];
            if ((e.flags & FLAG_OCCUPIED) && (e.flags & FLAG_DIRTY)) {
                list.push_back(idx | TOMBSTONE_BIT);
            }
        }
        if (!list.empty()) {
            total_dirty += list.size();
            non_empty_shards.push_back(s);
        }
    }

    // Sort dirty entries within each shard in exact LevelDB key order.
    auto sort_shard = [&](size_t s) {
        const Shard& shard = m_shards[s];
        std::shared_lock lock{shard.mutex};
        auto& list = shard_dirty[s];
        std::sort(list.begin(), list.end(), [&](uint32_t a, uint32_t b) noexcept {
            return CompareOutPointsLevelDBOrder(shard.KeyAt(a), shard.KeyAt(b));
        });
    };

    if (pool && pool->WorkersCount() > 0 && total_dirty >= 4096 && non_empty_shards.size() >= 4) {
        std::atomic<size_t> next_idx{0};
        const size_t num_helpers = std::min(pool->WorkersCount(), non_empty_shards.size() - 1);
        auto worker_fn = [&]() noexcept {
            while (true) {
                const size_t i = next_idx.fetch_add(1, std::memory_order_relaxed);
                if (i >= non_empty_shards.size()) break;
                sort_shard(non_empty_shards[i]);
            }
        };
        std::vector<std::function<void()>> tasks(num_helpers, worker_fn);
        if (auto futures = pool->SubmitPriority(std::move(tasks))) {
            worker_fn();
            for (auto& f : *futures) f.get();
        } else {
            worker_fn();
        }
    } else {
        for (size_t s : non_empty_shards) {
            sort_shard(s);
        }
    }

    // Stream sorted entries to base BEFORE clearing any shard so readers never see missing data on failure.
    base->BatchWriteEntries(total_dirty, total_entries, block_hash, [&](CoinsWriteSink& sink) {
        for (size_t s : non_empty_shards) {
            const Shard& shard = m_shards[s];
            std::shared_lock lock{shard.mutex};
            const auto& list = shard_dirty[s];
            const size_t n = list.size();
            for (size_t i = 0; i < n; ++i) {
                if (i + 4 < n) {
                    shard.PrefetchEntry(list[i + 4]);
                }
                const uint32_t tagged = list[i];
                if (tagged & TOMBSTONE_BIT) {
                    sink.Erase(shard.tomb_storage[tagged & INDEX_MASK].outpoint);
                } else {
                    const LiveEntry& entry = shard.live_storage[tagged];
                    sink.Write(entry.outpoint, entry.coin.ToCoin());
                }
            }
        }
    });

    // LevelDB write succeeded; clear all shards, then advance flush epoch so no reader
    // can observe the new epoch while stale pre-flush entries still exist in any shard.
    for (size_t s = 0; s < NUM_SHARDS; ++s) {
        Shard& shard = m_shards[s];
        std::unique_lock lock{shard.mutex};
        shard.Clear(reallocate_cache);
        SyncShardAccounting(shard);
        if (reallocate_cache) {
            std::vector<ShardUndoEntry>{}.swap(m_commit_undos[s].entries);
        } else {
            m_commit_undos[s].entries.clear();
        }
    }
    m_flush_epoch.fetch_add(1, std::memory_order_release);
}

void ShardedCoinsView::Sync(ThreadPool* pool)
{
    const uint256 block_hash = GetBestBlock();

    // O(dirty): only inspect shards with non-empty dirty slot lists.
    std::array<std::vector<uint32_t>, NUM_SHARDS> shard_dirty;
    std::vector<size_t> touched_shards;
    std::vector<size_t> emit_shards;
    touched_shards.reserve(NUM_SHARDS);
    emit_shards.reserve(NUM_SHARDS);
    size_t total_dirty = 0;
    size_t total_entries = m_cached_coins_count.load(std::memory_order_relaxed);
    bool removed_tombstones = false;

    for (size_t s = 0; s < NUM_SHARDS; ++s) {
        const Shard& shard = m_shards[s];
        std::shared_lock lock{shard.mutex};
        if (shard.dirty_live_slots.empty() && shard.dirty_tomb_slots.empty()) continue;
        touched_shards.push_back(s);

        auto& list = shard_dirty[s];
        list.reserve(shard.dirty_count);
        for (uint32_t idx : shard.dirty_live_slots) {
            const LiveEntry& e = shard.live_storage[idx];
            if ((e.Flags() & FLAG_OCCUPIED) && (e.Flags() & FLAG_DIRTY)) {
                list.push_back(idx);
            }
        }
        for (uint32_t idx : shard.dirty_tomb_slots) {
            const TombstoneEntry& e = shard.tomb_storage[idx];
            if ((e.flags & FLAG_OCCUPIED) && (e.flags & FLAG_DIRTY)) {
                list.push_back(idx | TOMBSTONE_BIT);
                removed_tombstones = true;
            }
        }
        if (!list.empty()) {
            total_dirty += list.size();
            emit_shards.push_back(s);
        }
    }

    auto sort_shard = [&](size_t s) {
        const Shard& shard = m_shards[s];
        std::shared_lock lock{shard.mutex};
        auto& list = shard_dirty[s];
        std::sort(list.begin(), list.end(), [&](uint32_t a, uint32_t b) noexcept {
            return CompareOutPointsLevelDBOrder(shard.KeyAt(a), shard.KeyAt(b));
        });
    };

    if (pool && pool->WorkersCount() > 0 && total_dirty >= 4096 && emit_shards.size() >= 4) {
        std::atomic<size_t> next_idx{0};
        const size_t num_helpers = std::min(pool->WorkersCount(), emit_shards.size() - 1);
        auto worker_fn = [&]() noexcept {
            while (true) {
                const size_t i = next_idx.fetch_add(1, std::memory_order_relaxed);
                if (i >= emit_shards.size()) break;
                sort_shard(emit_shards[i]);
            }
        };
        std::vector<std::function<void()>> tasks(num_helpers, worker_fn);
        if (auto futures = pool->SubmitPriority(std::move(tasks))) {
            worker_fn();
            for (auto& f : *futures) f.get();
        } else {
            worker_fn();
        }
    } else {
        for (size_t s : emit_shards) {
            sort_shard(s);
        }
    }

    // Stream dirty writes to base BEFORE mutating flags or erasing tombstones.
    base->BatchWriteEntries(total_dirty, total_entries, block_hash, [&](CoinsWriteSink& sink) {
        for (size_t s : emit_shards) {
            const Shard& shard = m_shards[s];
            std::shared_lock lock{shard.mutex};
            const auto& list = shard_dirty[s];
            const size_t n = list.size();
            for (size_t i = 0; i < n; ++i) {
                if (i + 4 < n) {
                    shard.PrefetchEntry(list[i + 4]);
                }
                const uint32_t tagged = list[i];
                if (tagged & TOMBSTONE_BIT) {
                    sink.Erase(shard.tomb_storage[tagged & INDEX_MASK].outpoint);
                } else {
                    const LiveEntry& entry = shard.live_storage[tagged];
                    sink.Write(entry.outpoint, entry.coin.ToCoin());
                }
            }
        }
    });

    // Drain dirty lists: clean live entries and backward-shift delete flushed tombstones.
    for (size_t s : touched_shards) {
        Shard& shard = m_shards[s];
        std::unique_lock lock{shard.mutex};

        for (uint32_t idx : shard.dirty_live_slots) {
            LiveEntry& entry = shard.live_storage[idx];
            if ((entry.Flags() & FLAG_OCCUPIED) && (entry.Flags() & FLAG_DIRTY)) {
                Assume(TrySub(shard.dirty_count, size_t{1}));
            }
            entry.AndFlags(static_cast<uint8_t>(~(FLAG_DIRTY | FLAG_FRESH | FLAG_IN_DIRTY_LIST)));
        }
        shard.dirty_live_slots.clear();

        for (uint32_t idx : shard.dirty_tomb_slots) {
            TombstoneEntry& entry = shard.tomb_storage[idx];
            if (entry.flags & FLAG_OCCUPIED) {
                const uint32_t hash32 = HashOutPoint(entry.outpoint);
                const size_t pos = shard.FindPos(entry.outpoint, hash32);
                Assert(pos != Shard::NOT_FOUND && shard.table[pos].tagged_idx == (idx | TOMBSTONE_BIT));
                shard.EraseIndexPos(pos);
                entry.flags &= static_cast<uint8_t>(~FLAG_IN_DIRTY_LIST);
                shard.FreeTombSlot(idx);
            } else {
                entry.flags &= static_cast<uint8_t>(~FLAG_IN_DIRTY_LIST);
            }
        }
        shard.dirty_tomb_slots.clear();
        SyncShardAccounting(shard);
    }

    if (removed_tombstones) {
        m_flush_epoch.fetch_add(1, std::memory_order_release);
    }
}

bool ShardedCoinsView::ApplyBatchSpend(
    Shard& shard,
    uint32_t block_idx,
    const COutPoint& outpoint,
    uint32_t hash32,
    bool from_db,
    bool epoch_matches,
    uint32_t expected_height,
    bool expected_coinbase,
    bool check_meta,
    std::vector<ShardUndoEntry>& undo) const
{
    const size_t pos = shard.FindPos(outpoint, hash32);
    if (pos != Shard::NOT_FOUND) {
        const uint32_t tagged = shard.table[pos].tagged_idx;
        if (tagged & TOMBSTONE_BIT) return false;
        LiveEntry& entry = shard.live_storage[tagged];
        if (check_meta && (entry.coin.Height() != expected_height || entry.coin.IsCoinBase() != expected_coinbase)) {
            return false;
        }
        const uint8_t prev_flags = entry.Flags() & (FLAG_DIRTY | FLAG_FRESH);
        StoredCoin old_coin;
        if (prev_flags & FLAG_FRESH) {
            shard.EraseIndexPos(pos);
            shard.FreeLiveSlotMoveStored(tagged, old_coin);
            undo.push_back(ShardUndoEntry{block_idx, RestoreKind::RESTORE_LIVE, prev_flags, outpoint, std::move(old_coin)});
        } else {
            const uint32_t tomb_idx = shard.AllocTombSlot(outpoint, FLAG_DIRTY);
            shard.table[pos].tagged_idx = tomb_idx | TOMBSTONE_BIT;
            shard.FreeLiveSlotMoveStored(tagged, old_coin);
            undo.push_back(ShardUndoEntry{block_idx, RestoreKind::RESTORE_LIVE_FROM_TOMB, prev_flags, outpoint, std::move(old_coin)});
        }
        return true;
    }

    bool in_db = false;
    if (epoch_matches) {
        in_db = from_db;
    } else if (check_meta) {
        const auto db_coin = base->PeekCoin(outpoint);
        in_db = db_coin.has_value() &&
                db_coin->nHeight == expected_height &&
                db_coin->IsCoinBase() == expected_coinbase;
    } else {
        in_db = base->HaveCoin(outpoint);
    }
    if (!in_db) return false;

    const uint32_t tomb_idx = shard.AllocTombSlot(outpoint, FLAG_DIRTY);
    shard.InsertIndexSlot(tomb_idx | TOMBSTONE_BIT, hash32);
    undo.push_back(ShardUndoEntry{block_idx, RestoreKind::ERASE_TOMB, 0, outpoint, StoredCoin{}});
    return true;
}

bool ShardedCoinsView::ApplyBatchCreate(
    Shard& shard,
    uint32_t block_idx,
    const COutPoint& outpoint,
    uint32_t hash32,
    const CTxOut& out,
    uint32_t height,
    bool is_coinbase,
    bool possible_overwrite,
    std::vector<ShardUndoEntry>& undo) const
{
    Assert(!out.IsNull());
    if (out.scriptPubKey.IsUnspendable()) return true;

    const size_t pos = shard.FindPos(outpoint, hash32);
    if (pos != Shard::NOT_FOUND) {
        const uint32_t tagged = shard.table[pos].tagged_idx;
        if (tagged & TOMBSTONE_BIT) {
            const uint32_t old_tomb = tagged & INDEX_MASK;
            const uint8_t prev_flags = shard.tomb_storage[old_tomb].flags & FLAG_DIRTY;
            const uint32_t live_idx = shard.AllocLiveSlot(outpoint, out, height, is_coinbase, FLAG_DIRTY);
            shard.table[pos].tagged_idx = live_idx;
            shard.FreeTombSlot(old_tomb);
            undo.push_back(ShardUndoEntry{block_idx, RestoreKind::RESTORE_TOMB_FROM_LIVE, prev_flags, outpoint, StoredCoin{}});
            return true;
        }
        if (!possible_overwrite) return false;
        LiveEntry& entry = shard.live_storage[tagged];
        const uint8_t prev_flags = entry.Flags() & (FLAG_DIRTY | FLAG_FRESH);
        Assume(TrySub(shard.script_dynamic_bytes, entry.coin.DynamicMemoryUsage()));
        StoredCoin old_coin;
        old_coin.AssignPreservingFlags(std::move(entry.coin));
        entry.coin.AssignPreservingFlags(out, height, is_coinbase);
        shard.script_dynamic_bytes += entry.coin.DynamicMemoryUsage();
        entry.AndFlags(static_cast<uint8_t>(~FLAG_FRESH));
        shard.MarkLiveDirty(tagged);
        undo.push_back(ShardUndoEntry{block_idx, RestoreKind::RESTORE_LIVE, prev_flags, outpoint, std::move(old_coin)});
        return true;
    }

    const uint8_t flags = static_cast<uint8_t>(FLAG_DIRTY | (!possible_overwrite ? FLAG_FRESH : 0));
    const uint32_t live_idx = shard.AllocLiveSlot(outpoint, out, height, is_coinbase, flags);
    shard.InsertIndexSlot(live_idx, hash32);
    undo.push_back(ShardUndoEntry{block_idx, RestoreKind::ERASE_LIVE, 0, outpoint, StoredCoin{}});
    return true;
}

bool ShardedCoinsView::ApplyBatchCreate(
    Shard& shard,
    uint32_t block_idx,
    const COutPoint& outpoint,
    uint32_t hash32,
    const Coin& coin,
    bool possible_overwrite,
    std::vector<ShardUndoEntry>& undo) const
{
    return ApplyBatchCreate(shard, block_idx, outpoint, hash32, coin.out, coin.nHeight, coin.fCoinBase, possible_overwrite, undo);
}

bool ShardedCoinsView::RollbackShardUndo(
    Shard& shard,
    uint32_t committed,
    std::vector<ShardUndoEntry>& undo) const
{
    if (undo.empty() || undo.back().block_idx < committed) return false;

    std::unique_lock lock{shard.mutex};
    bool erased_tombstone = false;

    while (!undo.empty() && undo.back().block_idx >= committed) {
        ShardUndoEntry u = std::move(undo.back());
        undo.pop_back();

        const uint32_t hash32 = HashOutPoint(u.outpoint);
        const size_t pos = shard.FindPos(u.outpoint, hash32);

        switch (u.kind) {
        case RestoreKind::ERASE_LIVE: {
            Assert(pos != Shard::NOT_FOUND && !(shard.table[pos].tagged_idx & TOMBSTONE_BIT));
            const uint32_t live_idx = shard.table[pos].tagged_idx;
            shard.EraseIndexPos(pos);
            shard.FreeLiveSlot(live_idx);
            break;
        }
        case RestoreKind::ERASE_TOMB: {
            Assert(pos != Shard::NOT_FOUND && (shard.table[pos].tagged_idx & TOMBSTONE_BIT));
            const uint32_t tomb_idx = shard.table[pos].tagged_idx & INDEX_MASK;
            shard.EraseIndexPos(pos);
            shard.FreeTombSlot(tomb_idx);
            erased_tombstone = true;
            break;
        }
        case RestoreKind::RESTORE_LIVE: {
            if (pos == Shard::NOT_FOUND) {
                const uint32_t live_idx = shard.AllocLiveSlotStored(u.outpoint, std::move(u.coin), u.prev_flags);
                shard.InsertIndexSlot(live_idx, hash32);
            } else {
                Assert(!(shard.table[pos].tagged_idx & TOMBSTONE_BIT));
                const uint32_t live_idx = shard.table[pos].tagged_idx;
                LiveEntry& entry = shard.live_storage[live_idx];
                Assume(TrySub(shard.script_dynamic_bytes, entry.coin.DynamicMemoryUsage()));
                const bool was_dirty = (entry.Flags() & FLAG_DIRTY) != 0;
                entry.coin.AssignPreservingFlags(std::move(u.coin));
                shard.script_dynamic_bytes += entry.coin.DynamicMemoryUsage();
                const uint8_t keep_list = entry.Flags() & FLAG_IN_DIRTY_LIST;
                entry.SetFlags(static_cast<uint8_t>(keep_list | FLAG_OCCUPIED | (u.prev_flags & (FLAG_DIRTY | FLAG_FRESH))));
                const bool now_dirty = (entry.Flags() & FLAG_DIRTY) != 0;
                if (was_dirty && !now_dirty) {
                    Assume(TrySub(shard.dirty_count, size_t{1}));
                } else if (!was_dirty && now_dirty) {
                    ++shard.dirty_count;
                }
                if (now_dirty && !(entry.Flags() & FLAG_IN_DIRTY_LIST)) {
                    entry.OrFlags(FLAG_IN_DIRTY_LIST);
                    shard.dirty_live_slots.push_back(live_idx);
                }
            }
            break;
        }
        case RestoreKind::RESTORE_LIVE_FROM_TOMB: {
            Assert(pos != Shard::NOT_FOUND && (shard.table[pos].tagged_idx & TOMBSTONE_BIT));
            const uint32_t tomb_idx = shard.table[pos].tagged_idx & INDEX_MASK;
            const uint32_t live_idx = shard.AllocLiveSlotStored(u.outpoint, std::move(u.coin), u.prev_flags);
            shard.table[pos].tagged_idx = live_idx;
            shard.FreeTombSlot(tomb_idx);
            erased_tombstone = true;
            break;
        }
        case RestoreKind::RESTORE_TOMB_FROM_LIVE: {
            Assert(pos != Shard::NOT_FOUND && !(shard.table[pos].tagged_idx & TOMBSTONE_BIT));
            const uint32_t live_idx = shard.table[pos].tagged_idx;
            const uint32_t tomb_idx = shard.AllocTombSlot(u.outpoint, u.prev_flags);
            shard.table[pos].tagged_idx = tomb_idx | TOMBSTONE_BIT;
            shard.FreeLiveSlot(live_idx);
            break;
        }
        }
    }

    SyncShardAccounting(shard);
    return erased_tombstone;
}

size_t ShardedCoinsView::CommitBlockBatch(
    std::span<const BatchCommitBlockMeta> blocks,
    std::array<std::vector<BatchCommitOp>, NUM_SHARDS>& shard_ops,
    ThreadPool* pool)
{
    if (blocks.empty()) return 0;

    const uint64_t current_epoch = FlushEpoch();
    uint32_t initial_limit = static_cast<uint32_t>(blocks.size());
    for (uint32_t b = 0; b < static_cast<uint32_t>(blocks.size()); ++b) {
        if (blocks[b].enforce_bip30 && blocks[b].validated_flush_epoch != current_epoch) {
            initial_limit = b;
            break;
        }
    }
    if (initial_limit == 0) return 0;

    std::vector<size_t> active_shards;
    active_shards.reserve(NUM_SHARDS);
    size_t total_ops = 0;
    for (size_t s = 0; s < NUM_SHARDS; ++s) {
        if (!shard_ops[s].empty()) {
            active_shards.push_back(s);
            total_ops += shard_ops[s].size();
        }
    }

    if (active_shards.empty()) {
        SetBestBlock(blocks[initial_limit - 1].block_hash);
        return initial_limit;
    }

    std::array<AlignedUndoVec, NUM_SHARDS> shard_undos;
    std::atomic<uint32_t> min_conflict_block{initial_limit};

    auto apply_shard = [&](size_t s) {
        Shard& shard = m_shards[s];
        auto& ops = shard_ops[s];
        std::vector<ShardUndoEntry> undo;
        const size_t num_ops = ops.size();
        undo.reserve(num_ops);

        std::vector<uint32_t> hashes(num_ops);
        for (size_t i = 0; i < num_ops; ++i) {
            hashes[i] = HashOutPoint(ops[i].outpoint);
        }

        static constexpr size_t PREFETCH_TABLE_DIST = 8;
        static constexpr size_t PREFETCH_ENTRY_DIST = 4;

        std::unique_lock lock{shard.mutex};

        if (!shard.table.empty()) {
            const size_t prime_end = std::min(num_ops, PREFETCH_TABLE_DIST);
            for (size_t i = 0; i < prime_end; ++i) {
                shard.PrefetchTableSlot(hashes[i]);
            }
        }

        for (size_t i = 0; i < num_ops; ++i) {
            auto& op = ops[i];
            const uint32_t limit = min_conflict_block.load(std::memory_order_relaxed);
            if (op.block_idx >= limit) break;

            if (!shard.table.empty()) {
                if (i + PREFETCH_TABLE_DIST < num_ops) {
                    shard.PrefetchTableSlot(hashes[i + PREFETCH_TABLE_DIST]);
                }
                if (i + PREFETCH_ENTRY_DIST < num_ops) {
                    const uint32_t h_ahead = hashes[i + PREFETCH_ENTRY_DIST];
                    const IndexSlot& slot_ahead = shard.table[h_ahead & (shard.table.size() - 1)];
                    if (slot_ahead.tagged_idx != EMPTY_TAG && slot_ahead.hash32 == h_ahead) {
                        shard.PrefetchEntry(slot_ahead.tagged_idx);
                    }
                }
            }

            bool ok = false;
            if (op.kind == BatchCommitOp::Kind::SPEND) {
                const bool epoch_matches = blocks[op.block_idx].validated_flush_epoch == current_epoch;
                const bool check_meta = !op.coin.IsSpent();
                ok = ApplyBatchSpend(
                    shard, op.block_idx, op.outpoint, hashes[i], op.from_db, epoch_matches,
                    check_meta ? op.coin.nHeight : uint32_t{0}, check_meta ? op.coin.IsCoinBase() : false,
                    check_meta, undo);
            } else {
                ok = ApplyBatchCreate(shard, op.block_idx, op.outpoint, hashes[i], op.coin, op.possible_overwrite, undo);
            }
            if (!ok) {
                AtomicMin(min_conflict_block, op.block_idx);
                break;
            }
        }

        SyncShardAccounting(shard);
        shard_undos[s].entries = std::move(undo);
    };

    if (pool && pool->WorkersCount() > 0 && active_shards.size() >= 8 && total_ops >= 4096) {
        std::atomic<size_t> next_idx{0};
        const size_t num_helpers = std::min({pool->WorkersCount(), active_shards.size() - 1, total_ops / 1024});
        auto worker_fn = [&]() noexcept {
            // Any exception (e.g. OOM) during a partial shard commit cannot be safely recovered
            // without risking dangling stack captures or a half-applied UTXO tip; terminate.
            while (true) {
                const size_t i = next_idx.fetch_add(1, std::memory_order_relaxed);
                if (i >= active_shards.size()) break;
                apply_shard(active_shards[i]);
            }
        };
        // Per-helper atomic state: 0 = QUEUED, 1 = RUNNING, 2 = CANCELLED.
        // Allows the coordinator to cancel unstarted helpers without blocking in f.get().
        auto helper_states = std::shared_ptr<std::atomic<uint8_t>[]>(new std::atomic<uint8_t>[num_helpers]{});
        std::vector<std::function<void()>> tasks;
        tasks.reserve(num_helpers);
        for (size_t h = 0; h < num_helpers; ++h) {
            tasks.emplace_back([helper_states, h, &worker_fn]() noexcept {
                uint8_t expected = 0;
                if (!helper_states[h].compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) return;
                worker_fn();
            });
        }
        if (auto futures = pool->SubmitPriority(std::move(tasks))) {
            worker_fn();
            for (size_t h = 0; h < num_helpers; ++h) {
                uint8_t expected = 0;
                if (!helper_states[h].compare_exchange_strong(expected, 2, std::memory_order_acq_rel)) {
                    (*futures)[h].get();
                }
            }
        } else {
            worker_fn();
        }
    } else {
        try {
            for (size_t s : active_shards) {
                apply_shard(s);
            }
        } catch (...) {
            std::terminate();
        }
    }

    const uint32_t committed = min_conflict_block.load(std::memory_order_acquire);

    if (committed < initial_limit) {
        bool erased_tombstone = false;
        try {
            for (size_t s : active_shards) {
                if (RollbackShardUndo(m_shards[s], committed, shard_undos[s].entries)) {
                    erased_tombstone = true;
                }
            }
        } catch (...) {
            std::terminate();
        }
        if (erased_tombstone) {
            m_flush_epoch.fetch_add(1, std::memory_order_release);
        }
    }

    if (committed > 0) {
        SetBestBlock(blocks[committed - 1].block_hash);
    }
    return committed;
}

size_t ShardedCoinsView::CommitBlockBatch(
    std::span<const ShardedBlockCommit> blocks,
    ThreadPool* pool)
{
    if (blocks.empty()) return 0;

    const uint64_t current_epoch = FlushEpoch();
    uint32_t initial_limit = static_cast<uint32_t>(blocks.size());
    for (uint32_t b = 0; b < static_cast<uint32_t>(blocks.size()); ++b) {
        if (blocks[b].meta.enforce_bip30 && blocks[b].meta.validated_flush_epoch != current_epoch) {
            initial_limit = b;
            break;
        }
    }
    if (initial_limit == 0) return 0;

    std::array<uint32_t, NUM_SHARDS> shard_op_counts{};
    size_t total_ops = 0;
    for (uint32_t b = 0; b < initial_limit; ++b) {
        const auto& sp_off = *Assert(blocks[b].spend_offsets);
        const auto& cr_off = *Assert(blocks[b].create_offsets);
        for (size_t s = 0; s < NUM_SHARDS; ++s) {
            const uint32_t cnt = (sp_off[s + 1] - sp_off[s]) + (cr_off[s + 1] - cr_off[s]);
            shard_op_counts[s] += cnt;
            total_ops += cnt;
        }
    }

    std::vector<size_t> active_shards;
    active_shards.reserve(NUM_SHARDS);
    for (size_t s = 0; s < NUM_SHARDS; ++s) {
        if (shard_op_counts[s] > 0) {
            active_shards.push_back(s);
        }
    }

    if (active_shards.empty()) {
        SetBestBlock(blocks[initial_limit - 1].meta.block_hash);
        return initial_limit;
    }

    std::atomic<uint32_t> min_conflict_block{initial_limit};

    auto apply_shard = [&](size_t s) {
        Shard& shard = m_shards[s];
        const size_t num_ops = shard_op_counts[s];
        auto& undo = m_commit_undos[s].entries;
        undo.clear();
        if (undo.capacity() < num_ops) undo.reserve(num_ops);

        // Precompute outpoint hashes in exact (block_idx, SPEND-then-CREATE) order outside shard lock.
        static thread_local std::vector<uint32_t> hashes;
        hashes.clear();
        if (hashes.capacity() < num_ops) hashes.reserve(num_ops);
        for (uint32_t b = 0; b < initial_limit; ++b) {
            const auto& blk = blocks[b];
            const uint32_t sb = (*blk.spend_offsets)[s];
            const uint32_t se = (*blk.spend_offsets)[s + 1];
            for (uint32_t i = sb; i < se; ++i) {
                hashes.push_back(HashOutPoint(blk.spends[i].outpoint));
            }
            const uint32_t cb = (*blk.create_offsets)[s];
            const uint32_t ce = (*blk.create_offsets)[s + 1];
            for (uint32_t i = cb; i < ce; ++i) {
                const BlockCreateOp& cr = blk.creates[i];
                hashes.push_back(HashOutPoint(COutPoint{cr.tx->GetHash(), cr.vout_idx}));
            }
        }

        static constexpr size_t PREFETCH_TABLE_DIST = 8;
        static constexpr size_t PREFETCH_ENTRY_DIST = 4;

        std::unique_lock lock{shard.mutex};

        if (!shard.table.empty()) {
            const size_t prime_end = std::min(num_ops, PREFETCH_TABLE_DIST);
            for (size_t i = 0; i < prime_end; ++i) {
                shard.PrefetchTableSlot(hashes[i]);
            }
        }

        auto prefetch_ahead = [&](size_t h_idx) noexcept {
            if (!shard.table.empty()) {
                if (h_idx + PREFETCH_TABLE_DIST < num_ops) {
                    shard.PrefetchTableSlot(hashes[h_idx + PREFETCH_TABLE_DIST]);
                }
                if (h_idx + PREFETCH_ENTRY_DIST < num_ops) {
                    const uint32_t h_ahead = hashes[h_idx + PREFETCH_ENTRY_DIST];
                    const IndexSlot& slot_ahead = shard.table[h_ahead & (shard.table.size() - 1)];
                    if (slot_ahead.tagged_idx != EMPTY_TAG && slot_ahead.hash32 == h_ahead) {
                        shard.PrefetchEntry(slot_ahead.tagged_idx);
                    }
                }
            }
        };

        size_t h_idx = 0;
        bool conflicted = false;
        for (uint32_t b = 0; b < initial_limit && !conflicted; ++b) {
            if (b >= min_conflict_block.load(std::memory_order_relaxed)) break;
            const auto& blk = blocks[b];
            const bool epoch_matches = blk.meta.validated_flush_epoch == current_epoch;

            const uint32_t sb = (*blk.spend_offsets)[s];
            const uint32_t se = (*blk.spend_offsets)[s + 1];
            for (uint32_t i = sb; i < se; ++i, ++h_idx) {
                prefetch_ahead(h_idx);
                const BlockSpendOp& sp = blk.spends[i];
                if (!ApplyBatchSpend(shard, b, sp.outpoint, hashes[h_idx], sp.from_db, epoch_matches,
                                     sp.expected_height, sp.expected_coinbase != 0, /*check_meta=*/true, undo)) {
                    AtomicMin(min_conflict_block, b);
                    conflicted = true;
                    break;
                }
            }
            if (conflicted) break;

            const uint32_t cb = (*blk.create_offsets)[s];
            const uint32_t ce = (*blk.create_offsets)[s + 1];
            for (uint32_t i = cb; i < ce; ++i, ++h_idx) {
                prefetch_ahead(h_idx);
                const BlockCreateOp& cr = blk.creates[i];
                const COutPoint outpoint{cr.tx->GetHash(), cr.vout_idx};
                if (!ApplyBatchCreate(shard, b, outpoint, hashes[h_idx], cr.tx->vout[cr.vout_idx],
                                      blk.meta.height, cr.is_coinbase != 0, cr.possible_overwrite != 0, undo)) {
                    AtomicMin(min_conflict_block, b);
                    conflicted = true;
                    break;
                }
            }
        }

        SyncShardAccounting(shard);
    };

    const bool ran_parallel = pool && pool->WorkersCount() > 0 && active_shards.size() >= 8 && total_ops >= 4096;
    if (ran_parallel) {
        std::atomic<size_t> next_idx{0};
        const size_t num_helpers = std::min({pool->WorkersCount(), active_shards.size() - 1, total_ops / 1024});
        auto worker_fn = [&]() noexcept {
            // Any exception (e.g. OOM) during a partial shard commit cannot be safely recovered
            // without risking dangling stack captures or a half-applied UTXO tip; terminate.
            while (true) {
                const size_t i = next_idx.fetch_add(1, std::memory_order_relaxed);
                if (i >= active_shards.size()) break;
                apply_shard(active_shards[i]);
            }
        };
        // Per-helper atomic state: 0 = QUEUED, 1 = RUNNING, 2 = CANCELLED.
        // Allows the coordinator to cancel unstarted helpers without blocking in f.get().
        auto helper_states = std::shared_ptr<std::atomic<uint8_t>[]>(new std::atomic<uint8_t>[num_helpers]{});
        std::vector<std::function<void()>> tasks;
        tasks.reserve(num_helpers);
        for (size_t h = 0; h < num_helpers; ++h) {
            tasks.emplace_back([helper_states, h, &worker_fn]() noexcept {
                uint8_t expected = 0;
                if (!helper_states[h].compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) return;
                worker_fn();
            });
        }
        if (auto futures = pool->SubmitPriority(std::move(tasks))) {
            worker_fn();
            for (size_t h = 0; h < num_helpers; ++h) {
                uint8_t expected = 0;
                if (!helper_states[h].compare_exchange_strong(expected, 2, std::memory_order_acq_rel)) {
                    (*futures)[h].get();
                }
            }
        } else {
            worker_fn();
        }
    } else {
        try {
            for (size_t s : active_shards) {
                apply_shard(s);
            }
        } catch (...) {
            std::terminate();
        }
    }

    const uint32_t committed = min_conflict_block.load(std::memory_order_acquire);

    if (committed < initial_limit) {
        bool erased_tombstone = false;
        try {
            for (size_t s : active_shards) {
                if (RollbackShardUndo(m_shards[s], committed, m_commit_undos[s].entries)) {
                    erased_tombstone = true;
                }
            }
        } catch (...) {
            std::terminate();
        }
        if (erased_tombstone) {
            m_flush_epoch.fetch_add(1, std::memory_order_release);
        }
    }

    // Always drop spent coin payloads after commit/rollback and cap retained vector capacity
    // so large batches do not leave uncounted memory outside DynamicMemoryUsage().
    static constexpr size_t MAX_RETAINED_UNDO_CAP = 512;
    for (size_t s : active_shards) {
        auto& entries = m_commit_undos[s].entries;
        entries.clear();
        if (entries.capacity() > MAX_RETAINED_UNDO_CAP) {
            std::vector<ShardUndoEntry>{}.swap(entries);
        }
    }

    if (committed > 0) {
        SetBestBlock(blocks[committed - 1].meta.block_hash);
    }
    return committed;
}

bool ShardedCoinsView::HaveInputs(const CTransaction& tx) const
{
    if (!tx.IsCoinBase()) {
        for (const CTxIn& txin : tx.vin) {
            if (!HaveCoin(txin.prevout)) return false;
        }
    }
    return true;
}

void ShardedCoinsView::SanityCheck() const
{
    size_t total_entries = 0;
    size_t total_dirty = 0;
    size_t total_mem = 0;

    for (size_t s = 0; s < NUM_SHARDS; ++s) {
        const Shard& shard = m_shards[s];
        std::shared_lock lock{shard.mutex};

        size_t table_live = 0;
        size_t table_tomb = 0;
        size_t table_dirty = 0;
        size_t script_bytes = 0;

        for (size_t pos = 0; pos < shard.table.size(); ++pos) {
            const IndexSlot& slot = shard.table[pos];
            if (slot.tagged_idx == EMPTY_TAG) continue;
            const COutPoint& key = shard.KeyAt(slot.tagged_idx);
            assert(ShardIndex(key) == s);
            const uint32_t h = HashOutPoint(key);
            assert(slot.hash32 == h);
            assert(shard.FindPos(key, h) == pos);

            if (slot.tagged_idx & TOMBSTONE_BIT) {
                const uint32_t idx = slot.tagged_idx & INDEX_MASK;
                const TombstoneEntry& e = shard.tomb_storage[idx];
                assert(e.flags & FLAG_OCCUPIED);
                assert(e.flags & FLAG_DIRTY);
                assert(e.flags & FLAG_IN_DIRTY_LIST);
                ++table_tomb;
                ++table_dirty;
            } else {
                const LiveEntry& e = shard.live_storage[slot.tagged_idx];
                assert(e.Flags() & FLAG_OCCUPIED);
                assert(!e.coin.IsSpent());
                assert((e.Flags() & FLAG_DIRTY) || !(e.Flags() & FLAG_FRESH));
                if (e.Flags() & FLAG_DIRTY) {
                    assert(e.Flags() & FLAG_IN_DIRTY_LIST);
                    ++table_dirty;
                }
                script_bytes += e.coin.DynamicMemoryUsage();
                ++table_live;
            }
        }

        assert(table_live == shard.live_count);
        assert(table_tomb == shard.tomb_count);
        assert(table_dirty == shard.dirty_count);
        assert(script_bytes == shard.script_dynamic_bytes);
        assert(shard.reported_coins_count == shard.EntryCount());
        assert(shard.reported_dirty_count == shard.dirty_count);
        assert(shard.reported_memory_usage == shard.ComputeMemoryUsage());

        // Verify no duplicate indices in dirty_live_slots / dirty_tomb_slots and every entry has FLAG_IN_DIRTY_LIST.
        std::vector<uint32_t> live_sorted = shard.dirty_live_slots;
        std::sort(live_sorted.begin(), live_sorted.end());
        assert(std::adjacent_find(live_sorted.begin(), live_sorted.end()) == live_sorted.end());
        for (uint32_t idx : live_sorted) {
            assert(shard.live_storage[idx].Flags() & FLAG_IN_DIRTY_LIST);
        }

        std::vector<uint32_t> tomb_sorted = shard.dirty_tomb_slots;
        std::sort(tomb_sorted.begin(), tomb_sorted.end());
        assert(std::adjacent_find(tomb_sorted.begin(), tomb_sorted.end()) == tomb_sorted.end());
        for (uint32_t idx : tomb_sorted) {
            assert(shard.tomb_storage[idx].flags & FLAG_IN_DIRTY_LIST);
        }

        total_entries += shard.EntryCount();
        total_dirty += shard.dirty_count;
        total_mem += shard.ComputeMemoryUsage();
    }

    assert(total_entries == m_cached_coins_count.load(std::memory_order_relaxed));
    assert(total_dirty == m_dirty_count.load(std::memory_order_relaxed));
    assert(total_mem == m_dynamic_memory_usage.load(std::memory_order_relaxed));
}
