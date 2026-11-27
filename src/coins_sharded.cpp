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
            SyncShardAccounting(shard);
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
        auto worker_fn = [&]() {
            while (true) {
                const size_t i = next_idx.fetch_add(1, std::memory_order_relaxed);
                if (i >= non_empty_shards.size()) break;
                sort_shard(non_empty_shards[i]);
            }
        };
        std::vector<std::function<void()>> tasks(num_helpers, worker_fn);
        if (auto futures = pool->SubmitPriority(std::move(tasks))) {
            worker_fn();
            for (auto& f : *futures) f.wait();
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

    // LevelDB write succeeded; clear all shards.
    for (size_t s = 0; s < NUM_SHARDS; ++s) {
        Shard& shard = m_shards[s];
        std::unique_lock lock{shard.mutex};
        shard.Clear(reallocate_cache);
        SyncShardAccounting(shard);
    }
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
        auto worker_fn = [&]() {
            while (true) {
                const size_t i = next_idx.fetch_add(1, std::memory_order_relaxed);
                if (i >= emit_shards.size()) break;
                sort_shard(emit_shards[i]);
            }
        };
        std::vector<std::function<void()>> tasks(num_helpers, worker_fn);
        if (auto futures = pool->SubmitPriority(std::move(tasks))) {
            worker_fn();
            for (auto& f : *futures) f.wait();
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
