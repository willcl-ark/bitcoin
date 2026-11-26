// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <coinsstore.h>

#include <memusage.h>
#include <util/check.h>
#include <util/hasher.h>
#include <util/threadpool.h>

#include <algorithm>
#include <bit>
#include <cassert>
#include <future>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {

constexpr size_t PARALLEL_FLUSH_MIN_DIRTY{4096};
constexpr size_t MAX_PARALLEL_FLUSH_BATCH{16};

inline bool GetBit(uint64_t bits, uint32_t offset) noexcept
{
    return (bits & (uint64_t{1} << offset)) != 0;
}

inline void SetBit(uint64_t& bits, uint32_t offset) noexcept
{
    bits |= (uint64_t{1} << offset);
}

inline void ClearBit(uint64_t& bits, uint32_t offset) noexcept
{
    bits &= ~(uint64_t{1} << offset);
}

inline uint32_t MakeCoinCode(uint32_t height, bool coinbase) noexcept
{
    return (height << 1) | uint32_t{coinbase};
}

} // namespace

CoinsStore::CoinsStore(CCoinsView* base_view, size_t shards, bool deterministic)
    : CCoinsViewBacked(base_view),
      m_hasher{deterministic},
      m_shards(std::max<size_t>(1, shards))
{
}

CoinsStore::~CoinsStore() = default;

bool CoinsStore::IsLiveDirty(const Shard& shard, uint32_t pos)
{
    return GetBit(shard.live_chunks[pos >> CHUNK_SHIFT]->dirty, pos & CHUNK_MASK);
}

bool CoinsStore::IsLiveFresh(const Shard& shard, uint32_t pos)
{
    return GetBit(shard.live_chunks[pos >> CHUNK_SHIFT]->fresh, pos & CHUNK_MASK);
}

void CoinsStore::SetDirtyChunkBit(Shard& shard, size_t chunk_idx)
{
    const size_t word{chunk_idx >> 6};
    if (word >= shard.dirty_chunk_mask.size()) shard.dirty_chunk_mask.resize(word + 1, 0);
    shard.dirty_chunk_mask[word] |= (uint64_t{1} << (chunk_idx & 63));
}

void CoinsStore::MarkLiveDirty(Shard& shard, uint32_t pos)
{
    const size_t c{pos >> CHUNK_SHIFT};
    const uint32_t o{pos & CHUNK_MASK};
    LiveChunk& chunk{*shard.live_chunks[c]};
    if (GetBit(chunk.dirty, o)) return;
    SetBit(chunk.dirty, o);
    SetDirtyChunkBit(shard, c);
    ++shard.dirty_live_count;
}

void CoinsStore::ClearLiveDirty(Shard& shard, uint32_t pos)
{
    const size_t c{pos >> CHUNK_SHIFT};
    const uint32_t o{pos & CHUNK_MASK};
    LiveChunk& chunk{*shard.live_chunks[c]};
    if (!GetBit(chunk.dirty, o)) return;
    ClearBit(chunk.dirty, o);
    Assert(shard.dirty_live_count > 0);
    --shard.dirty_live_count;
}

void CoinsStore::SetLiveFresh(Shard& shard, uint32_t pos, bool fresh)
{
    const size_t c{pos >> CHUNK_SHIFT};
    const uint32_t o{pos & CHUNK_MASK};
    LiveChunk& chunk{*shard.live_chunks[c]};
    if (fresh) {
        SetBit(chunk.fresh, o);
        SetDirtyChunkBit(shard, c);
    } else {
        ClearBit(chunk.fresh, o);
    }
}

uint32_t CoinsStore::AppendLive(Shard& shard, const COutPoint& key)
{
    Assert(shard.live_count < INDEX_MASK);
    const uint32_t pos{shard.live_count++};
    const size_t c{pos >> CHUNK_SHIFT};
    if (c >= shard.live_chunks.size()) {
        shard.live_chunks.push_back(std::make_unique<LiveChunk>());
    }
    LiveChunk& chunk{*shard.live_chunks[c]};
    const uint32_t o{pos & CHUNK_MASK};
    chunk.entries[o].key = key;
    chunk.entries[o].code = 0;
    chunk.entries[o].out.SetNull();
    ClearBit(chunk.dirty, o);
    ClearBit(chunk.fresh, o);
    return pos;
}

uint32_t CoinsStore::AppendTomb(Shard& shard, const COutPoint& key)
{
    Assert(shard.tomb_count < INDEX_MASK);
    const uint32_t pos{shard.tomb_count++};
    const size_t c{pos >> CHUNK_SHIFT};
    if (c >= shard.tomb_chunks.size()) {
        shard.tomb_chunks.push_back(std::make_unique<TombChunk>());
    }
    shard.tomb_chunks[c]->keys[pos & CHUNK_MASK] = key;
    return pos;
}

void CoinsStore::EraseLiveStorage(Shard& shard, uint32_t pos) const
{
    AssertLockHeld(shard.mutex);
    Assert(shard.live_count > 0);
    const uint32_t last{--shard.live_count};
    if (pos != last) {
        const size_t dst_c{pos >> CHUNK_SHIFT};
        const uint32_t dst_o{pos & CHUNK_MASK};
        const size_t src_c{last >> CHUNK_SHIFT};
        const uint32_t src_o{last & CHUNK_MASK};
        LiveChunk& dst_chunk{*shard.live_chunks[dst_c]};
        LiveChunk& src_chunk{*shard.live_chunks[src_c]};

        dst_chunk.entries[dst_o] = std::move(src_chunk.entries[src_o]);
        src_chunk.entries[src_o].out.nValue = -1;
        CScript{}.swap(src_chunk.entries[src_o].out.scriptPubKey);

        const bool was_dirty{GetBit(src_chunk.dirty, src_o)};
        const bool was_fresh{GetBit(src_chunk.fresh, src_o)};
        if (was_dirty) {
            SetBit(dst_chunk.dirty, dst_o);
            SetDirtyChunkBit(shard, dst_c);
        } else {
            ClearBit(dst_chunk.dirty, dst_o);
        }
        if (was_fresh) {
            SetBit(dst_chunk.fresh, dst_o);
            SetDirtyChunkBit(shard, dst_c);
        } else {
            ClearBit(dst_chunk.fresh, dst_o);
        }
        ClearBit(src_chunk.dirty, src_o);
        ClearBit(src_chunk.fresh, src_o);

        const size_t mask{shard.index.size() - 1};
        size_t at{HashKey(dst_chunk.entries[dst_o].key) & mask};
        while (shard.index[at] != last) at = (at + 1) & mask;
        shard.index[at] = pos;
    } else {
        const size_t c{pos >> CHUNK_SHIFT};
        const uint32_t o{pos & CHUNK_MASK};
        LiveChunk& chunk{*shard.live_chunks[c]};
        chunk.entries[o].out.nValue = -1;
        CScript{}.swap(chunk.entries[o].out.scriptPubKey);
        ClearBit(chunk.dirty, o);
        ClearBit(chunk.fresh, o);
    }
    const size_t needed{(shard.live_count + CHUNK_SIZE - 1) >> CHUNK_SHIFT};
    while (shard.live_chunks.size() > needed + 1) {
        shard.live_chunks.pop_back();
    }
}

void CoinsStore::EraseTombStorage(Shard& shard, uint32_t tomb_pos) const
{
    AssertLockHeld(shard.mutex);
    Assert(shard.tomb_count > 0);
    const uint32_t last{--shard.tomb_count};
    if (tomb_pos != last) {
        const COutPoint moved_key{TombAt(shard, last)};
        TombAt(shard, tomb_pos) = moved_key;
        const size_t mask{shard.index.size() - 1};
        const uint32_t old_tagged{TOMB_BIT | last};
        size_t at{HashKey(moved_key) & mask};
        while (shard.index[at] != old_tagged) at = (at + 1) & mask;
        shard.index[at] = TOMB_BIT | tomb_pos;
    }
    const size_t needed{(shard.tomb_count + CHUNK_SIZE - 1) >> CHUNK_SHIFT};
    while (shard.tomb_chunks.size() > needed + 1) {
        shard.tomb_chunks.pop_back();
    }
}

size_t CoinsStore::FindIndexSlot(const Shard& shard, const COutPoint& key, size_t hash) const
{
    AssertLockHeld(shard.mutex);
    if (shard.index.empty()) return std::numeric_limits<size_t>::max();
    const size_t mask{shard.index.size() - 1};
    for (size_t at{hash & mask}; ; at = (at + 1) & mask) {
        const uint32_t tagged{shard.index[at]};
        if (tagged == EMPTY_SLOT) return std::numeric_limits<size_t>::max();
        if (SlotKey(shard, tagged) == key) return at;
    }
}

uint32_t CoinsStore::FindTagged(const Shard& shard, const COutPoint& key, size_t hash) const
{
    const size_t slot{FindIndexSlot(shard, key, hash)};
    return slot == std::numeric_limits<size_t>::max() ? EMPTY_SLOT : shard.index[slot];
}

void CoinsStore::Rehash(Shard& shard, size_t new_buckets) const
{
    AssertLockHeld(shard.mutex);
    new_buckets = std::bit_ceil(std::max<size_t>(16, new_buckets));
    shard.index.assign(new_buckets, EMPTY_SLOT);
    const size_t mask{new_buckets - 1};
    for (uint32_t i{0}; i < shard.live_count; ++i) {
        size_t at{HashKey(LiveAt(shard, i).key) & mask};
        while (shard.index[at] != EMPTY_SLOT) at = (at + 1) & mask;
        shard.index[at] = i;
    }
    for (uint32_t i{0}; i < shard.tomb_count; ++i) {
        size_t at{HashKey(TombAt(shard, i)) & mask};
        while (shard.index[at] != EMPTY_SLOT) at = (at + 1) & mask;
        shard.index[at] = TOMB_BIT | i;
    }
}

void CoinsStore::InsertIndexSlot(Shard& shard, const COutPoint& key, size_t hash, uint32_t tagged) const
{
    AssertLockHeld(shard.mutex);
    const size_t total{size_t{shard.live_count} + size_t{shard.tomb_count}};
    if (shard.index.empty() || total * 10 >= shard.index.size() * 7) {
        // `tagged` has already been appended to live_chunks or tomb_chunks, so Rehash() indexes it.
        Rehash(shard, shard.index.empty() ? 16 : shard.index.size() * 2);
        return;
    }
    const size_t mask{shard.index.size() - 1};
    size_t at{hash & mask};
    while (shard.index[at] != EMPTY_SLOT) at = (at + 1) & mask;
    shard.index[at] = tagged;
}

void CoinsStore::RemoveIndexSlot(Shard& shard, size_t hole) const
{
    AssertLockHeld(shard.mutex);
    const size_t mask{shard.index.size() - 1};
    for (size_t cur{(hole + 1) & mask}; shard.index[cur] != EMPTY_SLOT; cur = (cur + 1) & mask) {
        const uint32_t tagged{shard.index[cur]};
        const size_t home{HashKey(SlotKey(shard, tagged)) & mask};
        if (((hole - home) & mask) < ((cur - home) & mask)) {
            shard.index[hole] = tagged;
            hole = cur;
        }
    }
    shard.index[hole] = EMPTY_SLOT;
}

void CoinsStore::PublishStats(Shard& shard) const
{
    AssertLockHeld(shard.mutex);
    const size_t bytes{
        memusage::DynamicUsage(shard.live_chunks) + shard.live_chunks.size() * sizeof(LiveChunk) +
        memusage::DynamicUsage(shard.tomb_chunks) + shard.tomb_chunks.size() * sizeof(TombChunk) +
        memusage::DynamicUsage(shard.dirty_chunk_mask) +
        memusage::DynamicUsage(shard.index) +
        shard.script_bytes};
    shard.a_bytes.store(bytes, std::memory_order_relaxed);
    shard.a_entries.store(size_t{shard.live_count} + size_t{shard.tomb_count}, std::memory_order_relaxed);
    shard.a_dirty.store(shard.dirty_live_count + size_t{shard.tomb_count}, std::memory_order_relaxed);
}

void CoinsStore::AssignCoinMove(Shard& shard, PackedEntry& entry, uint32_t code, CAmount value, CScript&& script)
{
    shard.script_bytes -= memusage::DynamicUsage(entry.out.scriptPubKey);
    entry.code = code;
    entry.out.nValue = value;
    entry.out.scriptPubKey = std::move(script);
    shard.script_bytes += memusage::DynamicUsage(entry.out.scriptPubKey);
}

void CoinsStore::AssignCoinCopy(Shard& shard, PackedEntry& entry, uint32_t code, CAmount value, const CScript& script)
{
    shard.script_bytes -= memusage::DynamicUsage(entry.out.scriptPubKey);
    entry.code = code;
    entry.out.nValue = value;
    entry.out.scriptPubKey = script;
    shard.script_bytes += memusage::DynamicUsage(entry.out.scriptPubKey);
}

void CoinsStore::SpendLiveAtSlot(Shard& shard, size_t index_slot, Coin* moveto) const
{
    AssertLockHeld(shard.mutex);
    const uint32_t pos{shard.index[index_slot]};
    Assert((pos & TOMB_BIT) == 0);
    PackedEntry& entry{LiveAt(shard, pos)};
    const COutPoint key{entry.key};
    const bool was_fresh{IsLiveFresh(shard, pos)};
    shard.script_bytes -= memusage::DynamicUsage(entry.out.scriptPubKey);
    if (moveto) {
        moveto->out.nValue = entry.out.nValue;
        moveto->out.scriptPubKey = std::move(entry.out.scriptPubKey);
        moveto->nHeight = entry.Height();
        moveto->fCoinBase = entry.IsCoinBase();
    }
    ClearLiveDirty(shard, pos);
    SetLiveFresh(shard, pos, false);
    if (was_fresh) {
        RemoveIndexSlot(shard, index_slot);
        EraseLiveStorage(shard, pos);
        return;
    }
    // Convert live entry to a 36-byte tombstone in place in the index (no index shift/rehash needed).
    const uint32_t tomb_pos{AppendTomb(shard, key)};
    shard.index[index_slot] = TOMB_BIT | tomb_pos;
    EraseLiveStorage(shard, pos);
}

void CoinsStore::ReplaceTombWithLive(Shard& shard, size_t index_slot, uint32_t code, CAmount value, CScript&& script) const
{
    AssertLockHeld(shard.mutex);
    const uint32_t tagged{shard.index[index_slot]};
    Assert((tagged & TOMB_BIT) != 0);
    const uint32_t tomb_pos{tagged & INDEX_MASK};
    const COutPoint key{TombAt(shard, tomb_pos)};
    const uint32_t live_pos{AppendLive(shard, key)};
    AssignCoinMove(shard, LiveAt(shard, live_pos), code, value, std::move(script));
    // Re-adding over a dirty spent tombstone must stay non-FRESH so the base DB update is never dropped.
    SetLiveFresh(shard, live_pos, false);
    MarkLiveDirty(shard, live_pos);
    shard.index[index_slot] = live_pos;
    EraseTombStorage(shard, tomb_pos);
}

void CoinsStore::ReplaceTombWithLiveCopy(Shard& shard, size_t index_slot, uint32_t code, CAmount value, const CScript& script) const
{
    AssertLockHeld(shard.mutex);
    const uint32_t tagged{shard.index[index_slot]};
    Assert((tagged & TOMB_BIT) != 0);
    const uint32_t tomb_pos{tagged & INDEX_MASK};
    const COutPoint key{TombAt(shard, tomb_pos)};
    const uint32_t live_pos{AppendLive(shard, key)};
    AssignCoinCopy(shard, LiveAt(shard, live_pos), code, value, script);
    SetLiveFresh(shard, live_pos, false);
    MarkLiveDirty(shard, live_pos);
    shard.index[index_slot] = live_pos;
    EraseTombStorage(shard, tomb_pos);
}

std::optional<Coin> CoinsStore::Lookup(const COutPoint& outpoint) const
{
    Shard& shard{ShardFor(outpoint)};
    const size_t hash{HashKey(outpoint)};
    while (true) {
        uint64_t gen;
        {
            LOCK(shard.mutex);
            const uint32_t tagged{FindTagged(shard, outpoint, hash)};
            if (tagged != EMPTY_SLOT) {
                if (tagged & TOMB_BIT) return std::nullopt;
                return LiveAt(shard, tagged).ToCoin();
            }
            gen = shard.generation;
        }
        std::optional<Coin> coin{base->GetCoin(outpoint)};
        LOCK(shard.mutex);
        if (shard.generation != gen) continue;
        if (const uint32_t tagged{FindTagged(shard, outpoint, hash)}; tagged != EMPTY_SLOT) {
            if (tagged & TOMB_BIT) return std::nullopt;
            return LiveAt(shard, tagged).ToCoin();
        }
        if (!coin) return std::nullopt;
        Assert(!coin->IsSpent());
        const uint32_t pos{AppendLive(shard, outpoint)};
        AssignCoinCopy(shard, LiveAt(shard, pos), MakeCoinCode(coin->nHeight, coin->fCoinBase), coin->out.nValue, coin->out.scriptPubKey);
        InsertIndexSlot(shard, outpoint, hash, pos);
        PublishStats(shard);
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
    const size_t hash{HashKey(outpoint)};
    {
        LOCK(shard.mutex);
        if (const uint32_t tagged{FindTagged(shard, outpoint, hash)}; tagged != EMPTY_SLOT) {
            if (tagged & TOMB_BIT) return std::nullopt;
            return LiveAt(shard, tagged).ToCoin();
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
    const size_t hash{HashKey(outpoint)};
    LOCK(shard.mutex);
    const uint32_t tagged{FindTagged(shard, outpoint, hash)};
    return tagged != EMPTY_SLOT && (tagged & TOMB_BIT) == 0;
}

uint256 CoinsStore::GetBestBlock() const
{
    return m_store_block_hash.IsNull() ? base->GetBestBlock() : m_store_block_hash;
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
    const size_t hash{HashKey(outpoint)};
    const uint32_t code{MakeCoinCode(coin.nHeight, coin.fCoinBase)};
    LOCK(shard.mutex);
    const size_t slot{FindIndexSlot(shard, outpoint, hash)};
    if (slot == std::numeric_limits<size_t>::max()) {
        const uint32_t pos{AppendLive(shard, outpoint)};
        AssignCoinMove(shard, LiveAt(shard, pos), code, coin.out.nValue, std::move(coin.out.scriptPubKey));
        SetLiveFresh(shard, pos, !possible_overwrite);
        MarkLiveDirty(shard, pos);
        InsertIndexSlot(shard, outpoint, hash, pos);
        PublishStats(shard);
        return;
    }
    const uint32_t tagged{shard.index[slot]};
    if (tagged & TOMB_BIT) {
        ReplaceTombWithLive(shard, slot, code, coin.out.nValue, std::move(coin.out.scriptPubKey));
        PublishStats(shard);
        return;
    }
    if (!possible_overwrite) {
        throw std::logic_error("Attempted to overwrite an unspent coin (when possible_overwrite is false)");
    }
    AssignCoinMove(shard, LiveAt(shard, tagged), code, coin.out.nValue, std::move(coin.out.scriptPubKey));
    SetLiveFresh(shard, tagged, false);
    MarkLiveDirty(shard, tagged);
    PublishStats(shard);
}

void CoinsStore::EmplaceCoinInternalDANGER(const COutPoint& outpoint, Coin&& coin)
{
    Shard& shard{ShardFor(outpoint)};
    const size_t hash{HashKey(outpoint)};
    LOCK(shard.mutex);
    if (FindIndexSlot(shard, outpoint, hash) != std::numeric_limits<size_t>::max()) return;
    const uint32_t pos{AppendLive(shard, outpoint)};
    AssignCoinMove(shard, LiveAt(shard, pos), MakeCoinCode(coin.nHeight, coin.fCoinBase), coin.out.nValue, std::move(coin.out.scriptPubKey));
    SetLiveFresh(shard, pos, false);
    MarkLiveDirty(shard, pos);
    InsertIndexSlot(shard, outpoint, hash, pos);
    PublishStats(shard);
}

bool CoinsStore::SpendCoin(const COutPoint& outpoint, Coin* moveto)
{
    Shard& shard{ShardFor(outpoint)};
    const size_t hash{HashKey(outpoint)};
    {
        LOCK(shard.mutex);
        if (const size_t slot{FindIndexSlot(shard, outpoint, hash)}; slot != std::numeric_limits<size_t>::max()) {
            if (shard.index[slot] & TOMB_BIT) {
                if (moveto) moveto->Clear();
                return true;
            }
            SpendLiveAtSlot(shard, slot, moveto);
            PublishStats(shard);
            return true;
        }
    }
    if (!Lookup(outpoint)) return false;
    LOCK(shard.mutex);
    const size_t slot{FindIndexSlot(shard, outpoint, hash)};
    if (slot == std::numeric_limits<size_t>::max()) return false;
    if (shard.index[slot] & TOMB_BIT) {
        if (moveto) moveto->Clear();
        return true;
    }
    SpendLiveAtSlot(shard, slot, moveto);
    PublishStats(shard);
    return true;
}

void CoinsStore::Uncache(const COutPoint& outpoint)
{
    Shard& shard{ShardFor(outpoint)};
    const size_t hash{HashKey(outpoint)};
    LOCK(shard.mutex);
    const size_t slot{FindIndexSlot(shard, outpoint, hash)};
    if (slot == std::numeric_limits<size_t>::max()) return;
    const uint32_t tagged{shard.index[slot]};
    // Tombstones are always dirty and cannot be uncached.
    if ((tagged & TOMB_BIT) || IsLiveDirty(shard, tagged)) return;
    const PackedEntry& entry{LiveAt(shard, tagged)};
    CCoinsViewCache::TraceUncache(outpoint, entry.Height(), entry.out.nValue, entry.IsCoinBase());
    shard.script_bytes -= memusage::DynamicUsage(entry.out.scriptPubKey);
    RemoveIndexSlot(shard, slot);
    EraseLiveStorage(shard, tagged);
    PublishStats(shard);
}

void CoinsStore::BatchWrite(CoinsViewCacheCursor& cursor, const uint256& block_hash)
{
    // Collect dirty pointers first (without erasing from cursor yet) and group by shard so each
    // shard mutex is acquired at most once for the whole batch.
    std::vector<CoinsCachePair*> dirty_entries;
    dirty_entries.reserve(cursor.GetDirtyCount());
    for (auto it{cursor.Begin()}; it != cursor.End(); it = it->second.Next()) {
        if (it->second.IsDirty()) dirty_entries.push_back(it);
    }

    if (!dirty_entries.empty()) {
        const size_t shards{m_shards.size()};
        std::vector<uint32_t> shard_of;
        shard_of.reserve(dirty_entries.size());
        std::vector<uint32_t> starts(shards + 1, 0);
        for (const CoinsCachePair* p : dirty_entries) {
            const uint32_t s{static_cast<uint32_t>(ShardIndex(p->first))};
            shard_of.push_back(s);
            ++starts[s + 1];
        }
        for (size_t s{0}; s < shards; ++s) starts[s + 1] += starts[s];
        std::vector<CoinsCachePair*> ordered(dirty_entries.size());
        {
            std::vector<uint32_t> next(starts.begin(), starts.end() - 1);
            for (size_t i{0}; i < dirty_entries.size(); ++i) {
                ordered[next[shard_of[i]]++] = dirty_entries[i];
            }
        }

        for (size_t s{0}; s < shards; ++s) {
            if (starts[s] == starts[s + 1]) continue;
            Shard& shard{m_shards[s]};
            LOCK(shard.mutex);
            for (uint32_t k{starts[s]}; k < starts[s + 1]; ++k) {
                CoinsCachePair& pair{*ordered[k]};
                const COutPoint& outpoint{pair.first};
                const size_t hash{HashKey(outpoint)};
                const bool child_spent{pair.second.coin.IsSpent()};
                const bool child_fresh{pair.second.IsFresh()};
                const bool move_coin{cursor.WillErase(pair)};
                const size_t slot{FindIndexSlot(shard, outpoint, hash)};

                if (slot == std::numeric_limits<size_t>::max()) {
                    if (child_fresh && child_spent) continue;
                    if (child_spent) {
                        const uint32_t tomb_pos{AppendTomb(shard, outpoint)};
                        InsertIndexSlot(shard, outpoint, hash, TOMB_BIT | tomb_pos);
                    } else {
                        const uint32_t pos{AppendLive(shard, outpoint)};
                        const uint32_t code{MakeCoinCode(pair.second.coin.nHeight, pair.second.coin.fCoinBase)};
                        if (move_coin) {
                            AssignCoinMove(shard, LiveAt(shard, pos), code, pair.second.coin.out.nValue, std::move(pair.second.coin.out.scriptPubKey));
                        } else {
                            AssignCoinCopy(shard, LiveAt(shard, pos), code, pair.second.coin.out.nValue, pair.second.coin.out.scriptPubKey);
                        }
                        SetLiveFresh(shard, pos, child_fresh);
                        MarkLiveDirty(shard, pos);
                        InsertIndexSlot(shard, outpoint, hash, pos);
                    }
                    continue;
                }

                const uint32_t tagged{shard.index[slot]};
                if (tagged & TOMB_BIT) {
                    if (child_spent) {
                        // Already a dirty tombstone.
                        continue;
                    }
                    const uint32_t code{MakeCoinCode(pair.second.coin.nHeight, pair.second.coin.fCoinBase)};
                    if (move_coin) {
                        ReplaceTombWithLive(shard, slot, code, pair.second.coin.out.nValue, std::move(pair.second.coin.out.scriptPubKey));
                    } else {
                        ReplaceTombWithLiveCopy(shard, slot, code, pair.second.coin.out.nValue, pair.second.coin.out.scriptPubKey);
                    }
                    continue;
                }

                // Existing live entry in CoinsStore.
                if (child_fresh) {
                    PublishStats(shard);
                    throw std::logic_error("FRESH flag misapplied to coin that exists in parent cache");
                }
                if (child_spent) {
                    SpendLiveAtSlot(shard, slot, /*moveto=*/nullptr);
                    continue;
                }
                const uint32_t code{MakeCoinCode(pair.second.coin.nHeight, pair.second.coin.fCoinBase)};
                if (move_coin) {
                    AssignCoinMove(shard, LiveAt(shard, tagged), code, pair.second.coin.out.nValue, std::move(pair.second.coin.out.scriptPubKey));
                } else {
                    AssignCoinCopy(shard, LiveAt(shard, tagged), code, pair.second.coin.out.nValue, pair.second.coin.out.scriptPubKey);
                }
                SetLiveFresh(shard, tagged, false);
                MarkLiveDirty(shard, tagged);
            }
            PublishStats(shard);
        }
    }

    for (auto it{cursor.Begin()}; it != cursor.End(); it = cursor.NextAndMaybeErase(*it)) {
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

void CoinsStore::FetchInto(std::span<const COutPoint> outpoints,
                           std::span<const uint32_t> which,
                           std::vector<Coin>& coins,
                           std::vector<uint8_t>& state) const
{
    state.assign(outpoints.size(), FETCH_ABSENT);
    if (which.empty()) return;
    std::vector<uint32_t> shard_of;
    shard_of.reserve(which.size());
    for (const uint32_t w : which) {
        shard_of.push_back(static_cast<uint32_t>(ShardIndex(outpoints[w])));
    }
    VisitShards(shard_of, [&](Shard& shard, std::span<const uint32_t> items) EXCLUSIVE_LOCKS_REQUIRED(shard.mutex) {
        if (shard.index.empty()) return;
        for (const uint32_t k : items) {
            const uint32_t w{which[k]};
            const uint32_t tagged{FindTagged(shard, outpoints[w], HashKey(outpoints[w]))};
            if (tagged == EMPTY_SLOT) continue;
            if (tagged & TOMB_BIT) {
                state[w] = FETCH_SPENT;
                continue;
            }
            coins[w] = LiveAt(shard, tagged).ToCoin();
            state[w] = FETCH_FOUND;
        }
    });
}

void CoinsStore::MayHave(std::span<const COutPoint> outpoints, std::vector<uint8_t>& may) const
{
    may.assign(outpoints.size(), 0);
    if (outpoints.empty()) return;
    std::vector<uint8_t> decided(outpoints.size(), 0);
    std::vector<uint32_t> shard_of;
    shard_of.reserve(outpoints.size());
    for (const COutPoint& outpoint : outpoints) {
        shard_of.push_back(static_cast<uint32_t>(ShardIndex(outpoint)));
    }
    VisitShards(shard_of, [&](Shard& shard, std::span<const uint32_t> items) EXCLUSIVE_LOCKS_REQUIRED(shard.mutex) {
        if (shard.index.empty()) return;
        for (const uint32_t i : items) {
            const uint32_t tagged{FindTagged(shard, outpoints[i], HashKey(outpoints[i]))};
            if (tagged == EMPTY_SLOT) continue;
            decided[i] = 1;
            if ((tagged & TOMB_BIT) == 0) may[i] = 1;
        }
    });
    std::vector<uint32_t> absent;
    for (uint32_t i{0}; i < outpoints.size(); ++i) {
        if (!decided[i]) absent.push_back(i);
    }
    if (absent.size() > 1) {
        std::sort(absent.begin(), absent.end(), [&](uint32_t a, uint32_t b) {
            return outpoints[a] < outpoints[b];
        });
    }
    for (const uint32_t i : absent) {
        if (base->HaveCoin(outpoints[i])) may[i] = 1;
    }
}

uint32_t CoinsStore::CheckShard(size_t shard_idx, std::span<const ShardOp> ops, uint32_t no_conflict) const
{
    if (ops.empty()) return no_conflict;
    Shard& shard{m_shards[shard_idx]};

    struct BaseHit {
        const COutPoint* key;
        bool exists;
        uint32_t code;
    };
    std::vector<BaseHit> fetched_base;
    auto find_fetched{[&](const COutPoint& key) -> const BaseHit* {
        if (fetched_base.empty()) return nullptr;
        const auto it{std::lower_bound(fetched_base.begin(), fetched_base.end(), key, [](const BaseHit& h, const COutPoint& k) { return *h.key < k; })};
        if (it != fetched_base.end() && *it->key == key) return &*it;
        return nullptr;
    }};

    // Simulate ops in strict block order against shard + local intra-batch overlay without mutating shard.
    struct LocalEntry {
        const COutPoint* key{nullptr};
        uint32_t code{0};
        bool spent{false};
    };
    const size_t table_size{std::bit_ceil(std::max<size_t>(16, ops.size() * 2))};
    const size_t local_mask{table_size - 1};
    std::vector<LocalEntry> local(table_size);

    auto find_local{[&](const COutPoint& key, size_t hash) -> LocalEntry* {
        for (size_t at{hash & local_mask}; local[at].key != nullptr; at = (at + 1) & local_mask) {
            if (*local[at].key == key) return &local[at];
        }
        return nullptr;
    }};
    auto put_local{[&](const COutPoint* key, size_t hash, uint32_t code, bool spent) {
        for (size_t at{hash & local_mask}; ; at = (at + 1) & local_mask) {
            if (local[at].key == nullptr || *local[at].key == *key) {
                local[at] = {key, code, spent};
                return;
            }
        }
    }};

    size_t idx{0};
    while (idx < ops.size()) {
        std::vector<const COutPoint*> to_fetch;
        {
            LOCK(shard.mutex);
            for (; idx < ops.size(); ++idx) {
                const ShardOp& op{ops[idx]};
                const LocalEntry* loc{find_local(op.outpoint, op.hash)};
                if (op.create_out == nullptr) {
                    // Spend: verify coin exists unspent with matching height/coinbase metadata.
                    bool ok{false};
                    if (loc != nullptr) {
                        ok = !loc->spent && loc->code == op.code;
                    } else {
                        const uint32_t tagged{FindTagged(shard, op.outpoint, op.hash)};
                        if (tagged != EMPTY_SLOT) {
                            ok = (tagged & TOMB_BIT) == 0 && LiveAt(shard, tagged).code == op.code;
                        } else if (op.trust_from_base) {
                            ok = true;
                        } else if (const BaseHit* hit{find_fetched(op.outpoint)}) {
                            ok = hit->exists && hit->code == op.code;
                        } else {
                            // Pause here and fetch remaining untrusted cache misses off shard.mutex in sorted key order.
                            for (size_t j{idx}; j < ops.size(); ++j) {
                                const ShardOp& rem{ops[j]};
                                if (rem.create_out != nullptr || rem.trust_from_base) continue;
                                if (find_local(rem.outpoint, rem.hash) != nullptr) continue;
                                if (find_fetched(rem.outpoint) != nullptr) continue;
                                if (FindTagged(shard, rem.outpoint, rem.hash) == EMPTY_SLOT) {
                                    to_fetch.push_back(&rem.outpoint);
                                }
                            }
                            break;
                        }
                    }
                    if (!ok) return op.block_idx;
                    put_local(&op.outpoint, op.hash, 0, /*spent=*/true);
                } else {
                    // Create: verify no live coin is overwritten unless possible_overwrite is set.
                    if (!op.possible_overwrite) {
                        bool live{false};
                        if (loc != nullptr) {
                            live = !loc->spent;
                        } else {
                            const uint32_t tagged{FindTagged(shard, op.outpoint, op.hash)};
                            live = (tagged != EMPTY_SLOT && (tagged & TOMB_BIT) == 0);
                        }
                        if (live) return op.block_idx;
                    }
                    put_local(&op.outpoint, op.hash, op.code, /*spent=*/false);
                }
            }
        }

        if (!to_fetch.empty()) {
            std::sort(to_fetch.begin(), to_fetch.end(), [](const COutPoint* a, const COutPoint* b) { return *a < *b; });
            to_fetch.erase(std::unique(to_fetch.begin(), to_fetch.end(), [](const COutPoint* a, const COutPoint* b) { return *a == *b; }), to_fetch.end());
            fetched_base.reserve(fetched_base.size() + to_fetch.size());
            for (const COutPoint* op : to_fetch) {
                if (const std::optional<Coin> coin{base->GetCoin(*op)}) {
                    fetched_base.push_back({op, true, MakeCoinCode(coin->nHeight, coin->fCoinBase)});
                } else {
                    fetched_base.push_back({op, false, 0});
                }
            }
            std::sort(fetched_base.begin(), fetched_base.end(), [](const BaseHit& a, const BaseHit& b) { return *a.key < *b.key; });
        }
    }
    return no_conflict;
}

void CoinsStore::ApplyShardUnconditional(size_t shard_idx, std::span<const ShardOp> ops, uint32_t valid_blocks)
{
    if (ops.empty() || valid_blocks == 0) return;
    Shard& shard{m_shards[shard_idx]};
    LOCK(shard.mutex);
    for (const ShardOp& op : ops) {
        if (op.block_idx >= valid_blocks) break;
        const size_t slot{FindIndexSlot(shard, op.outpoint, op.hash)};
        if (op.create_out == nullptr) {
            if (slot == std::numeric_limits<size_t>::max()) {
                // Verified in Phase 1 as existing in base DB; record spent tombstone.
                const uint32_t tomb_pos{AppendTomb(shard, op.outpoint)};
                InsertIndexSlot(shard, op.outpoint, op.hash, TOMB_BIT | tomb_pos);
            } else {
                Assert((shard.index[slot] & TOMB_BIT) == 0);
                SpendLiveAtSlot(shard, slot, /*moveto=*/nullptr);
            }
        } else {
            if (slot == std::numeric_limits<size_t>::max()) {
                const uint32_t pos{AppendLive(shard, op.outpoint)};
                AssignCoinCopy(shard, LiveAt(shard, pos), op.code, op.create_out->nValue, op.create_out->scriptPubKey);
                SetLiveFresh(shard, pos, !op.possible_overwrite);
                MarkLiveDirty(shard, pos);
                InsertIndexSlot(shard, op.outpoint, op.hash, pos);
            } else if (shard.index[slot] & TOMB_BIT) {
                ReplaceTombWithLiveCopy(shard, slot, op.code, op.create_out->nValue, op.create_out->scriptPubKey);
            } else {
                Assert(op.possible_overwrite);
                const uint32_t pos{shard.index[slot]};
                AssignCoinCopy(shard, LiveAt(shard, pos), op.code, op.create_out->nValue, op.create_out->scriptPubKey);
                SetLiveFresh(shard, pos, false);
                MarkLiveDirty(shard, pos);
            }
        }
    }
    PublishStats(shard);
}

void CoinsStore::CollectAndSortShard(const Shard& shard, std::vector<SortKey>& keys) const
{
    AssertLockHeld(shard.mutex);
    keys.clear();
    const size_t dirty_total{shard.dirty_live_count + size_t{shard.tomb_count}};
    if (dirty_total == 0) return;
    keys.reserve(dirty_total);

    for (size_t w{0}; w < shard.dirty_chunk_mask.size(); ++w) {
        uint64_t chunk_bits{shard.dirty_chunk_mask[w]};
        while (chunk_bits != 0) {
            const unsigned bit{static_cast<unsigned>(std::countr_zero(chunk_bits))};
            chunk_bits &= chunk_bits - 1;
            const size_t c{(w << 6) + bit};
            if (c >= shard.live_chunks.size()) continue;
            const LiveChunk& chunk{*shard.live_chunks[c]};
            const uint32_t base_pos{static_cast<uint32_t>(c << CHUNK_SHIFT)};
            uint64_t dirty_bits{chunk.dirty};
            while (dirty_bits != 0) {
                const unsigned o_bit{static_cast<unsigned>(std::countr_zero(dirty_bits))};
                dirty_bits &= dirty_bits - 1;
                const uint32_t pos{base_pos + static_cast<uint32_t>(o_bit)};
                if (pos >= shard.live_count) continue;
                const COutPoint& k{chunk.entries[o_bit].key};
                keys.push_back({ReadBE64(k.hash.ToUint256().data()), pos});
            }
        }
    }

    for (uint32_t t{0}; t < shard.tomb_count; ++t) {
        const COutPoint& k{TombAt(shard, t)};
        keys.push_back({ReadBE64(k.hash.ToUint256().data()), TOMB_BIT | t});
    }

    if (keys.size() >= 2) {
        std::sort(keys.begin(), keys.end(), [&](const SortKey& a, const SortKey& b) {
            if (a.prefix != b.prefix) return a.prefix > b.prefix;
            return SlotKey(shard, b.tagged) < SlotKey(shard, a.tagged);
        });
    }
}

void CoinsStore::EmitSortedShard(const Shard& shard,
                                 std::span<const SortKey> keys,
                                 std::vector<SortedCoinWrite>& writes,
                                 const std::function<void(std::span<const SortedCoinWrite>)>& emit) const
{
    AssertLockHeld(shard.mutex);
    if (keys.empty()) return;
    writes.clear();
    writes.reserve(keys.size());
    for (const SortKey& sk : keys) {
        if (sk.tagged & TOMB_BIT) {
            const COutPoint& k{TombAt(shard, sk.tagged & INDEX_MASK)};
            writes.push_back({&k, /*out=*/nullptr, /*code=*/0});
        } else {
            const PackedEntry& e{LiveAt(shard, sk.tagged)};
            writes.push_back({&e.key, &e.out, e.code});
        }
    }
    emit(writes);
}

void CoinsStore::WriteDirtyToBase(const uint256& best, ThreadPool* pool) const
{
    const size_t dirty_count{GetDirtyCount()};
    const size_t total_count{GetCacheSize()};
    const size_t workers{pool ? pool->WorkersCount() : 0};
    const bool use_parallel{workers > 0 && dirty_count >= PARALLEL_FLUSH_MIN_DIRTY};

    base->BatchWriteSorted(
        [&](const std::function<void(std::span<const SortedCoinWrite>)>& emit) {
            std::vector<SortedCoinWrite> writes;
            if (!use_parallel) {
                std::vector<SortKey> keys;
                for (size_t index{m_shards.size()}; index-- > 0;) {
                    Shard& shard{m_shards[index]};
                    LOCK(shard.mutex);
                    if (shard.dirty_live_count == 0 && shard.tomb_count == 0) continue;
                    CollectAndSortShard(shard, keys);
                    EmitSortedShard(shard, keys, writes, emit);
                }
                return;
            }

            // Double-buffered pipelined sort: sort up to `batch_size` shards in parallel on `pool`
            // while the calling thread streams the previously sorted batch into LevelDB.
            const size_t batch_size{std::clamp<size_t>(workers * 2, 2, MAX_PARALLEL_FLUSH_BATCH)};
            struct PendingBatch {
                std::vector<size_t> shard_indices;
                std::vector<std::vector<SortKey>> sorted;
                std::vector<std::future<void>> futures;
            };

            auto launch_batch{[&](size_t& next_remaining) -> PendingBatch {
                PendingBatch b;
                b.shard_indices.reserve(batch_size);
                while (next_remaining > 0 && b.shard_indices.size() < batch_size) {
                    const size_t idx{--next_remaining};
                    if (m_shards[idx].a_dirty.load(std::memory_order_relaxed) == 0) continue;
                    b.shard_indices.push_back(idx);
                }
                b.sorted.resize(b.shard_indices.size());
                b.futures.reserve(b.shard_indices.size());
                for (size_t i{0}; i < b.shard_indices.size(); ++i) {
                    const size_t idx{b.shard_indices[i]};
                    std::vector<SortKey>* out{&b.sorted[i]};
                    auto fut{pool->Submit([this, idx, out] {
                        WAITED_FOR_UNDER_LOCK("a coins flush sort");
                        Shard& shard{m_shards[idx]};
                        LOCK(shard.mutex);
                        CollectAndSortShard(shard, *out);
                    })};
                    if (fut) {
                        b.futures.push_back(std::move(*fut));
                    } else {
                        Shard& shard{m_shards[idx]};
                        LOCK(shard.mutex);
                        CollectAndSortShard(shard, *out);
                    }
                }
                return b;
            }};

            size_t next_remaining{m_shards.size()};
            PendingBatch current{launch_batch(next_remaining)};
            while (!current.shard_indices.empty()) {
                for (auto& f : current.futures) f.get();
                PendingBatch next{launch_batch(next_remaining)};
                for (size_t i{0}; i < current.shard_indices.size(); ++i) {
                    if (current.sorted[i].empty()) continue;
                    Shard& shard{m_shards[current.shard_indices[i]]};
                    LOCK(shard.mutex);
                    EmitSortedShard(shard, current.sorted[i], writes, emit);
                }
                current = std::move(next);
            }
        },
        best,
        dirty_count,
        total_count);
}

void CoinsStore::Flush(bool reallocate_cache, ThreadPool* pool)
{
    const uint256 best{GetBestBlock()};
    if (!best.IsNull()) {
        WriteDirtyToBase(best, pool);
    } else {
        Assert(GetDirtyCount() == 0);
    }
    for (Shard& shard : m_shards) {
        LOCK(shard.mutex);
        shard.live_chunks.clear();
        shard.tomb_chunks.clear();
        shard.dirty_chunk_mask.clear();
        shard.index.clear();
        if (reallocate_cache) {
            shard.live_chunks.shrink_to_fit();
            shard.tomb_chunks.shrink_to_fit();
            shard.dirty_chunk_mask.shrink_to_fit();
            shard.index.shrink_to_fit();
        }
        shard.live_count = 0;
        shard.tomb_count = 0;
        shard.dirty_live_count = 0;
        shard.script_bytes = 0;
        ++shard.generation;
        PublishStats(shard);
    }
    m_flush_gen.fetch_add(1, std::memory_order_acq_rel);
}

void CoinsStore::Sync(ThreadPool* pool)
{
    const uint256 best{GetBestBlock()};
    if (!best.IsNull()) {
        WriteDirtyToBase(best, pool);
    } else {
        Assert(GetDirtyCount() == 0);
    }
    for (Shard& shard : m_shards) {
        LOCK(shard.mutex);
        if (shard.dirty_live_count == 0 && shard.tomb_count == 0) {
            ++shard.generation;
            continue;
        }

        // Clear dirty/fresh bits only on chunks recorded in dirty_chunk_mask: O(dirty).
        for (size_t w{0}; w < shard.dirty_chunk_mask.size(); ++w) {
            uint64_t chunk_bits{shard.dirty_chunk_mask[w]};
            shard.dirty_chunk_mask[w] = 0;
            while (chunk_bits != 0) {
                const unsigned bit{static_cast<unsigned>(std::countr_zero(chunk_bits))};
                chunk_bits &= chunk_bits - 1;
                const size_t c{(w << 6) + bit};
                if (c < shard.live_chunks.size()) {
                    shard.live_chunks[c]->dirty = 0;
                    shard.live_chunks[c]->fresh = 0;
                }
            }
        }
        shard.dirty_live_count = 0;

        if (shard.tomb_count > 0) {
            if (shard.live_count == 0) {
                shard.tomb_chunks.clear();
                shard.tomb_count = 0;
                shard.index.clear();
            } else {
                const bool table_oversized{shard.index.size() > 64 && size_t{shard.live_count} * 8 < shard.index.size()};
                if (!table_oversized && size_t{shard.tomb_count} * 8 < shard.index.size()) {
                    // Remove tombstones in place via O(tomb_count) backward-shift deletion without rehashing.
                    const size_t mask{shard.index.size() - 1};
                    for (uint32_t t{0}; t < shard.tomb_count; ++t) {
                        const uint32_t target{TOMB_BIT | t};
                        size_t hole{HashKey(TombAt(shard, t)) & mask};
                        while (shard.index[hole] != target) hole = (hole + 1) & mask;
                        RemoveIndexSlot(shard, hole);
                    }
                    shard.tomb_chunks.clear();
                    shard.tomb_count = 0;
                } else {
                    shard.tomb_chunks.clear();
                    shard.tomb_count = 0;
                    Rehash(shard, std::max<size_t>(16, size_t{shard.live_count} * 10 / 7 + 1));
                }
            }
        } else if (shard.live_count == 0) {
            shard.index.clear();
        } else if (shard.index.size() > 64 && size_t{shard.live_count} * 8 < shard.index.size()) {
            Rehash(shard, std::max<size_t>(16, size_t{shard.live_count} * 10 / 7 + 1));
        }

        const size_t needed_live{(shard.live_count + CHUNK_SIZE - 1) >> CHUNK_SHIFT};
        shard.live_chunks.resize(needed_live);
        shard.dirty_chunk_mask.resize((needed_live + 63) >> 6, 0);
        ++shard.generation;
        PublishStats(shard);
    }
    m_flush_gen.fetch_add(1, std::memory_order_acq_rel);
}

unsigned int CoinsStore::GetCacheSize() const
{
    size_t total{0};
    for (const Shard& shard : m_shards) total += shard.a_entries.load(std::memory_order_relaxed);
    return static_cast<unsigned int>(total);
}

size_t CoinsStore::GetDirtyCount() const noexcept
{
    size_t total{0};
    for (const Shard& shard : m_shards) total += shard.a_dirty.load(std::memory_order_relaxed);
    return total;
}

size_t CoinsStore::DynamicMemoryUsage() const
{
    size_t total{0};
    for (const Shard& shard : m_shards) total += shard.a_bytes.load(std::memory_order_relaxed);
    return total;
}

void CoinsStore::SanityCheck() const
{
    size_t total_bytes{0};
    size_t total_entries{0};
    size_t total_dirty{0};
    for (const Shard& shard : m_shards) {
        LOCK(shard.mutex);
        size_t usage{0};
        size_t dirty_live{0};
        for (uint32_t i{0}; i < shard.live_count; ++i) {
            const PackedEntry& entry{LiveAt(shard, i)};
            assert(!entry.out.IsNull());
            usage += memusage::DynamicUsage(entry.out.scriptPubKey);
            const bool is_dirty{IsLiveDirty(shard, i)};
            const bool is_fresh{IsLiveFresh(shard, i)};
            if (is_dirty) {
                ++dirty_live;
                const size_t c{i >> CHUNK_SHIFT};
                assert((c >> 6) < shard.dirty_chunk_mask.size());
                assert((shard.dirty_chunk_mask[c >> 6] & (uint64_t{1} << (c & 63))) != 0);
            } else {
                assert(!is_fresh);
            }
            assert(FindTagged(shard, entry.key, HashKey(entry.key)) == i);
        }
        for (uint32_t t{0}; t < shard.tomb_count; ++t) {
            const COutPoint& key{TombAt(shard, t)};
            assert(FindTagged(shard, key, HashKey(key)) == (TOMB_BIT | t));
        }
        size_t occupied{0};
        for (const uint32_t slot : shard.index) {
            if (slot != EMPTY_SLOT) ++occupied;
        }
        assert(occupied == size_t{shard.live_count} + size_t{shard.tomb_count});
        assert(dirty_live == shard.dirty_live_count);
        assert(usage == shard.script_bytes);
        const size_t shard_bytes{
            memusage::DynamicUsage(shard.live_chunks) + shard.live_chunks.size() * sizeof(LiveChunk) +
            memusage::DynamicUsage(shard.tomb_chunks) + shard.tomb_chunks.size() * sizeof(TombChunk) +
            memusage::DynamicUsage(shard.dirty_chunk_mask) +
            memusage::DynamicUsage(shard.index) +
            usage};
        assert(shard.a_bytes.load(std::memory_order_relaxed) == shard_bytes);
        assert(shard.a_entries.load(std::memory_order_relaxed) == size_t{shard.live_count} + size_t{shard.tomb_count});
        assert(shard.a_dirty.load(std::memory_order_relaxed) == dirty_live + size_t{shard.tomb_count});
        total_bytes += shard_bytes;
        total_entries += size_t{shard.live_count} + size_t{shard.tomb_count};
        total_dirty += dirty_live + size_t{shard.tomb_count};
    }
    assert(DynamicMemoryUsage() == total_bytes);
    assert(GetCacheSize() == total_entries);
    assert(GetDirtyCount() == total_dirty);
}
