// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <coinsstore.h>

#include <memusage.h>
#include <util/check.h>
#include <util/hasher.h>

#include <algorithm>
#include <bit>
#include <cassert>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

bool IsCoin(const CoinsStore::Change& change, uint32_t height, bool coinbase)
{
    return height == change.coin->nHeight && coinbase == change.coin->fCoinBase;
}

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
      m_hasher{deterministic},
      m_shards(std::max<size_t>(1, shards))
{
}

CoinsStore::~CoinsStore() = default;

uint32_t CoinsStore::FindEntry(const Shard& shard, const COutPoint& key, size_t hash) const
{
    AssertLockHeld(shard.mutex);
    if (shard.index.empty()) return EMPTY_SLOT;
    const size_t mask{shard.index.size() - 1};
    for (size_t at{hash & mask}; ; at = (at + 1) & mask) {
        const uint32_t idx{shard.index[at]};
        if (idx == EMPTY_SLOT) return EMPTY_SLOT;
        if (shard.entries[idx].key == key) return idx;
    }
}

void CoinsStore::Rehash(Shard& shard, size_t new_buckets) const
{
    AssertLockHeld(shard.mutex);
    new_buckets = std::bit_ceil(std::max<size_t>(16, new_buckets));
    shard.index.assign(new_buckets, EMPTY_SLOT);
    const size_t mask{new_buckets - 1};
    for (uint32_t i{0}; i < shard.entries.size(); ++i) {
        size_t at{HashKey(shard.entries[i].key) & mask};
        while (shard.index[at] != EMPTY_SLOT) at = (at + 1) & mask;
        shard.index[at] = i;
    }
}

uint32_t CoinsStore::InsertOrFind(Shard& shard, const COutPoint& key, size_t hash, bool& inserted) const
{
    AssertLockHeld(shard.mutex);
    if (shard.index.empty() || (shard.entries.size() + 1) * 10 >= shard.index.size() * 7) {
        Rehash(shard, shard.index.empty() ? 16 : shard.index.size() * 2);
    }
    const size_t mask{shard.index.size() - 1};
    for (size_t at{hash & mask}; ; at = (at + 1) & mask) {
        const uint32_t idx{shard.index[at]};
        if (idx == EMPTY_SLOT) {
            const uint32_t pos{static_cast<uint32_t>(shard.entries.size())};
            PackedEntry& entry{shard.entries.emplace_back()};
            entry.key = key;
            shard.index[at] = pos;
            inserted = true;
            return pos;
        }
        if (shard.entries[idx].key == key) {
            inserted = false;
            return idx;
        }
    }
}

void CoinsStore::EraseEntry(Shard& shard, uint32_t pos) const
{
    AssertLockHeld(shard.mutex);
    const size_t mask{shard.index.size() - 1};
    size_t hole{HashKey(shard.entries[pos].key) & mask};
    while (shard.index[hole] != pos) hole = (hole + 1) & mask;

    // Backward-shift deletion preserves linear-probing invariants with zero tombstones.
    for (size_t cur{(hole + 1) & mask}; shard.index[cur] != EMPTY_SLOT; cur = (cur + 1) & mask) {
        const size_t home{HashKey(shard.entries[shard.index[cur]].key) & mask};
        if (((hole - home) & mask) < ((cur - home) & mask)) {
            shard.index[hole] = shard.index[cur];
            hole = cur;
        }
    }
    shard.index[hole] = EMPTY_SLOT;

    const uint32_t last{static_cast<uint32_t>(shard.entries.size() - 1)};
    if (pos != last) {
        shard.entries[pos] = std::move(shard.entries[last]);
        size_t at{HashKey(shard.entries[pos].key) & mask};
        while (shard.index[at] != last) at = (at + 1) & mask;
        shard.index[at] = pos;
    }
    shard.entries.pop_back();
}

void CoinsStore::PublishStats(Shard& shard) const
{
    AssertLockHeld(shard.mutex);
    const size_t bytes{memusage::DynamicUsage(shard.entries) + memusage::DynamicUsage(shard.index) + shard.script_bytes};
    shard.a_bytes.store(bytes, std::memory_order_relaxed);
    shard.a_entries.store(shard.entries.size(), std::memory_order_relaxed);
    shard.a_dirty.store(shard.dirty_count, std::memory_order_relaxed);
}

void CoinsStore::MarkDirty(Shard& shard, PackedEntry& entry) const
{
    AssertLockHeld(shard.mutex);
    if (entry.IsDirty()) return;
    entry.meta |= META_DIRTY;
    ++shard.dirty_count;
}

void CoinsStore::ClearDirty(Shard& shard, PackedEntry& entry) const
{
    AssertLockHeld(shard.mutex);
    if (!entry.IsDirty()) return;
    entry.meta &= ~META_DIRTY;
    Assert(shard.dirty_count > 0);
    --shard.dirty_count;
}

void CoinsStore::AssignCoin(Shard& shard, PackedEntry& entry, CAmount value, uint32_t height, bool coinbase, CScript&& script) const
{
    AssertLockHeld(shard.mutex);
    Assert(height <= MAX_PACKED_HEIGHT);
    shard.script_bytes -= memusage::DynamicUsage(entry.script);
    entry.value = value;
    entry.script = std::move(script);
    entry.meta = (entry.meta & (META_DIRTY | META_FRESH)) | (height << META_HEIGHT_SHIFT) | (coinbase ? META_COINBASE : 0U);
    shard.script_bytes += memusage::DynamicUsage(entry.script);
}

void CoinsStore::AssignCoinCopy(Shard& shard, PackedEntry& entry, CAmount value, uint32_t height, bool coinbase, const CScript& script) const
{
    AssertLockHeld(shard.mutex);
    Assert(height <= MAX_PACKED_HEIGHT);
    shard.script_bytes -= memusage::DynamicUsage(entry.script);
    entry.value = value;
    entry.script = script;
    entry.meta = (entry.meta & (META_DIRTY | META_FRESH)) | (height << META_HEIGHT_SHIFT) | (coinbase ? META_COINBASE : 0U);
    shard.script_bytes += memusage::DynamicUsage(entry.script);
}

void CoinsStore::SpendEntryLocked(Shard& shard, uint32_t pos, Coin* moveto) const
{
    AssertLockHeld(shard.mutex);
    PackedEntry& entry{shard.entries[pos]};
    shard.script_bytes -= memusage::DynamicUsage(entry.script);
    if (moveto) {
        if (entry.IsSpent()) {
            moveto->Clear();
        } else {
            moveto->out.nValue = entry.value;
            moveto->out.scriptPubKey = std::move(entry.script);
            moveto->nHeight = entry.Height();
            moveto->fCoinBase = entry.IsCoinBase();
        }
    }
    if (entry.IsFresh()) {
        ClearDirty(shard, entry);
        EraseEntry(shard, pos);
        return;
    }
    entry.ClearCoin();
    MarkDirty(shard, entry);
}

std::optional<Coin> CoinsStore::Lookup(const COutPoint& outpoint) const
{
    Shard& shard{ShardFor(outpoint)};
    const size_t hash{HashKey(outpoint)};
    while (true) {
        uint64_t gen;
        {
            LOCK(shard.mutex);
            const uint32_t pos{FindEntry(shard, outpoint, hash)};
            if (pos != EMPTY_SLOT) {
                const PackedEntry& entry{shard.entries[pos]};
                return entry.IsSpent() ? std::nullopt : std::make_optional(entry.ToCoin());
            }
            gen = shard.generation;
        }
        std::optional<Coin> coin{base->GetCoin(outpoint)};
        LOCK(shard.mutex);
        if (shard.generation != gen) continue;
        if (const uint32_t pos{FindEntry(shard, outpoint, hash)}; pos != EMPTY_SLOT) {
            const PackedEntry& entry{shard.entries[pos]};
            return entry.IsSpent() ? std::nullopt : std::make_optional(entry.ToCoin());
        }
        if (!coin) return std::nullopt;
        Assert(!coin->IsSpent());
        bool inserted{false};
        const uint32_t pos{InsertOrFind(shard, outpoint, hash, inserted)};
        Assert(inserted);
        AssignCoinCopy(shard, shard.entries[pos], coin->out.nValue, coin->nHeight, coin->fCoinBase, coin->out.scriptPubKey);
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
        if (const uint32_t pos{FindEntry(shard, outpoint, hash)}; pos != EMPTY_SLOT) {
            const PackedEntry& entry{shard.entries[pos]};
            return entry.IsSpent() ? std::nullopt : std::make_optional(entry.ToCoin());
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
    const uint32_t pos{FindEntry(shard, outpoint, hash)};
    return pos != EMPTY_SLOT && !shard.entries[pos].IsSpent();
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
    LOCK(shard.mutex);
    bool inserted{false};
    const uint32_t pos{InsertOrFind(shard, outpoint, hash, inserted)};
    PackedEntry& entry{shard.entries[pos]};
    bool fresh{false};
    if (!possible_overwrite) {
        if (!entry.IsSpent()) {
            PublishStats(shard);
            throw std::logic_error("Attempted to overwrite an unspent coin (when possible_overwrite is false)");
        }
        // A dirty spent entry records a pending erase from the base DB; re-adding must not mark it FRESH.
        fresh = !entry.IsDirty();
    }
    AssignCoin(shard, entry, coin.out.nValue, coin.nHeight, coin.fCoinBase, std::move(coin.out.scriptPubKey));
    if (fresh) {
        entry.meta |= META_FRESH;
    } else {
        entry.meta &= ~META_FRESH;
    }
    MarkDirty(shard, entry);
    PublishStats(shard);
}

void CoinsStore::EmplaceCoinInternalDANGER(const COutPoint& outpoint, Coin&& coin)
{
    Shard& shard{ShardFor(outpoint)};
    const size_t hash{HashKey(outpoint)};
    LOCK(shard.mutex);
    bool inserted{false};
    const uint32_t pos{InsertOrFind(shard, outpoint, hash, inserted)};
    if (!inserted) return;
    PackedEntry& entry{shard.entries[pos]};
    AssignCoin(shard, entry, coin.out.nValue, coin.nHeight, coin.fCoinBase, std::move(coin.out.scriptPubKey));
    entry.meta &= ~META_FRESH;
    MarkDirty(shard, entry);
    PublishStats(shard);
}

bool CoinsStore::SpendCoin(const COutPoint& outpoint, Coin* moveto)
{
    Shard& shard{ShardFor(outpoint)};
    const size_t hash{HashKey(outpoint)};
    {
        LOCK(shard.mutex);
        if (const uint32_t pos{FindEntry(shard, outpoint, hash)}; pos != EMPTY_SLOT) {
            SpendEntryLocked(shard, pos, moveto);
            PublishStats(shard);
            return true;
        }
    }
    if (!Lookup(outpoint)) return false;
    LOCK(shard.mutex);
    const uint32_t pos{FindEntry(shard, outpoint, hash)};
    if (pos == EMPTY_SLOT) return false;
    SpendEntryLocked(shard, pos, moveto);
    PublishStats(shard);
    return true;
}

void CoinsStore::Uncache(const COutPoint& outpoint)
{
    Shard& shard{ShardFor(outpoint)};
    const size_t hash{HashKey(outpoint)};
    LOCK(shard.mutex);
    const uint32_t pos{FindEntry(shard, outpoint, hash)};
    if (pos == EMPTY_SLOT || shard.entries[pos].IsDirty()) return;
    PackedEntry& entry{shard.entries[pos]};
    TraceUncache(outpoint, entry.ToCoin());
    shard.script_bytes -= memusage::DynamicUsage(entry.script);
    EraseEntry(shard, pos);
    PublishStats(shard);
}

void CoinsStore::BatchWrite(CoinsViewCacheCursor& cursor, const uint256& block_hash)
{
    for (auto it{cursor.Begin()}; it != cursor.End(); it = cursor.NextAndMaybeErase(*it)) {
        if (!it->second.IsDirty()) continue;
        const COutPoint& outpoint{it->first};
        Shard& shard{ShardFor(outpoint)};
        const size_t hash{HashKey(outpoint)};
        LOCK(shard.mutex);
        bool inserted{false};
        const uint32_t pos{InsertOrFind(shard, outpoint, hash, inserted)};
        PackedEntry& entry{shard.entries[pos]};
        if (inserted) {
            if (it->second.IsFresh() && it->second.coin.IsSpent()) {
                EraseEntry(shard, pos);
                PublishStats(shard);
                continue;
            }
            if (it->second.coin.IsSpent()) {
                entry.ClearCoin();
            } else if (cursor.WillErase(*it)) {
                AssignCoin(shard, entry, it->second.coin.out.nValue, it->second.coin.nHeight, it->second.coin.fCoinBase, std::move(it->second.coin.out.scriptPubKey));
            } else {
                AssignCoinCopy(shard, entry, it->second.coin.out.nValue, it->second.coin.nHeight, it->second.coin.fCoinBase, it->second.coin.out.scriptPubKey);
            }
            if (it->second.IsFresh() && !entry.IsSpent()) {
                entry.meta |= META_FRESH;
            } else {
                entry.meta &= ~META_FRESH;
            }
            MarkDirty(shard, entry);
            PublishStats(shard);
        } else {
            if (it->second.IsFresh() && !entry.IsSpent()) {
                throw std::logic_error("FRESH flag misapplied to coin that exists in parent cache");
            }
            if (entry.IsFresh() && it->second.coin.IsSpent()) {
                shard.script_bytes -= memusage::DynamicUsage(entry.script);
                ClearDirty(shard, entry);
                EraseEntry(shard, pos);
                PublishStats(shard);
                continue;
            }
            if (it->second.coin.IsSpent()) {
                shard.script_bytes -= memusage::DynamicUsage(entry.script);
                entry.ClearCoin();
            } else if (cursor.WillErase(*it)) {
                AssignCoin(shard, entry, it->second.coin.out.nValue, it->second.coin.nHeight, it->second.coin.fCoinBase, std::move(it->second.coin.out.scriptPubKey));
            } else {
                AssignCoinCopy(shard, entry, it->second.coin.out.nValue, it->second.coin.nHeight, it->second.coin.fCoinBase, it->second.coin.out.scriptPubKey);
            }
            entry.meta &= ~META_FRESH;
            MarkDirty(shard, entry);
            PublishStats(shard);
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

uint32_t CoinsStore::ApplyShard(size_t shard_idx,
                                std::span<const Change> changes,
                                ConfirmedReport* report,
                                const std::function<const Coin*(const COutPoint&)>& base_coin)
{
    if (changes.empty()) return 0;
    Shard& shard{m_shards[shard_idx]};
    uint32_t conflicts{0};
    std::unordered_map<COutPoint, std::optional<Coin>, SaltedOutpointHasher> fetched_base;
    size_t idx{0};

    while (idx < changes.size()) {
        std::vector<const COutPoint*> to_fetch;
        {
            LOCK(shard.mutex);
            for (; idx < changes.size(); ++idx) {
                const Change& change{changes[idx]};
                if (!change.coin->IsSpent() && (change.script ? *change.script : change.coin->out.scriptPubKey).IsUnspendable()) {
                    continue;
                }
                const COutPoint& outpoint{*change.outpoint};
                const size_t hash{HashKey(outpoint)};
                if (change.coin->IsSpent()) {
                    const uint32_t pos{FindEntry(shard, outpoint, hash)};
                    if (pos == EMPTY_SLOT) {
                        if (change.trust_from_base) {
                            bool inserted{false};
                            const uint32_t new_pos{InsertOrFind(shard, outpoint, hash, inserted)};
                            Assert(inserted);
                            PackedEntry& entry{shard.entries[new_pos]};
                            entry.ClearCoin();
                            entry.meta &= ~META_FRESH;
                            MarkDirty(shard, entry);
                        } else if (const auto it{fetched_base.find(outpoint)}; it != fetched_base.end()) {
                            const std::optional<Coin>& coin{it->second};
                            if (!coin || (change.known_coin && !IsCoin(*coin, *change.coin))) {
                                ++conflicts;
                                if (report) report->conflicted.push_back(change.outpoint);
                            } else {
                                bool inserted{false};
                                const uint32_t new_pos{InsertOrFind(shard, outpoint, hash, inserted)};
                                Assert(inserted);
                                PackedEntry& entry{shard.entries[new_pos]};
                                entry.ClearCoin();
                                entry.meta &= ~META_FRESH;
                                MarkDirty(shard, entry);
                            }
                        } else {
                            // Pause here and fetch all remaining untrusted spend misses off shard.mutex
                            // so every change in this shard still executes in strict input order.
                            for (size_t j{idx}; j < changes.size(); ++j) {
                                const Change& rem{changes[j]};
                                if (!rem.coin->IsSpent() || rem.trust_from_base) continue;
                                if (fetched_base.contains(*rem.outpoint)) continue;
                                if (FindEntry(shard, *rem.outpoint, HashKey(*rem.outpoint)) == EMPTY_SLOT) {
                                    to_fetch.push_back(rem.outpoint);
                                }
                            }
                            break;
                        }
                    } else {
                        const PackedEntry& entry{shard.entries[pos]};
                        if (entry.IsSpent() || (change.known_coin && !IsCoin(change, entry.Height(), entry.IsCoinBase()))) {
                            ++conflicts;
                            if (report) report->conflicted.push_back(change.outpoint);
                        } else {
                            SpendEntryLocked(shard, pos, /*moveto=*/nullptr);
                        }
                    }
                    continue;
                }
                bool inserted{false};
                const uint32_t pos{InsertOrFind(shard, outpoint, hash, inserted)};
                PackedEntry& entry{shard.entries[pos]};
                const bool over{!inserted && !entry.IsSpent()};
                if (over && !change.possible_overwrite) {
                    ++conflicts;
                    if (report) report->conflicted.push_back(change.outpoint);
                    continue;
                }
                if (over && report) report->overwritten.emplace_back(change.outpoint, entry.ToCoin());
                if (inserted && change.possible_overwrite && report) report->unread.push_back(change.outpoint);
                const bool fresh{!change.possible_overwrite && !entry.IsDirty()};
                if (change.script) {
                    AssignCoinCopy(shard, entry, change.coin->out.nValue, change.coin->nHeight, change.coin->fCoinBase, *change.script);
                } else {
                    AssignCoin(shard, entry, change.coin->out.nValue, change.coin->nHeight, change.coin->fCoinBase, std::move(change.coin->out.scriptPubKey));
                }
                if (fresh) {
                    entry.meta |= META_FRESH;
                } else {
                    entry.meta &= ~META_FRESH;
                }
                MarkDirty(shard, entry);
            }
            PublishStats(shard);
        }

        for (const COutPoint* outpoint : to_fetch) {
            if (fetched_base.contains(*outpoint)) continue;
            const Coin* const given{base_coin ? base_coin(*outpoint) : nullptr};
            fetched_base.emplace(*outpoint, given ? std::optional<Coin>{*given} : PeekCoin(*outpoint));
        }
    }

    return conflicts;
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

    const size_t shards{m_shards.size()};
    std::vector<uint32_t> starts(shards + 1, 0);
    for (const uint32_t s : shard_of) ++starts[s + 1];
    for (size_t s{0}; s < shards; ++s) starts[s + 1] += starts[s];
    std::vector<Change> ordered(changes.size());
    {
        std::vector<uint32_t> next(starts.begin(), starts.end() - 1);
        for (size_t i{0}; i < changes.size(); ++i) ordered[next[shard_of[i]]++] = changes[i];
    }

    uint32_t conflicts{0};
    for (size_t s{0}; s < shards; ++s) {
        if (starts[s] == starts[s + 1]) continue;
        conflicts += ApplyShard(s, std::span<const Change>{ordered}.subspan(starts[s], starts[s + 1] - starts[s]), report, base_coin);
    }
    return conflicts;
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
        if (shard.entries.empty()) return;
        for (const uint32_t k : items) {
            const uint32_t w{which[k]};
            const uint32_t pos{FindEntry(shard, outpoints[w], HashKey(outpoints[w]))};
            if (pos == EMPTY_SLOT || shard.entries[pos].IsSpent()) continue;
            coins[w] = shard.entries[pos].ToCoin();
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
        if (shard.entries.empty()) return;
        for (const uint32_t i : items) {
            if (FindEntry(shard, outpoints[i], HashKey(outpoints[i])) != EMPTY_SLOT) may[i] = 1;
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
                    if (shard.dirty_count == 0) continue;
                    writes.reserve(shard.dirty_count);
                    for (const PackedEntry& entry : shard.entries) {
                        if (!entry.IsDirty()) continue;
                        writes.emplace_back(entry.key, entry.IsSpent() ? std::nullopt : std::make_optional(entry.ToCoin()));
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
        shard.entries.clear();
        shard.index.clear();
        if (reallocate_cache) {
            shard.entries.shrink_to_fit();
            shard.index.shrink_to_fit();
        }
        shard.dirty_count = 0;
        shard.script_bytes = 0;
        ++shard.generation;
        PublishStats(shard);
    }
    m_flush_gen.fetch_add(1, std::memory_order_acq_rel);
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
        if (shard.dirty_count == 0) {
            ++shard.generation;
            continue;
        }
        size_t keep{0};
        for (size_t i{0}; i < shard.entries.size(); ++i) {
            PackedEntry& entry{shard.entries[i]};
            if (entry.IsDirty()) {
                entry.SetFlags(/*dirty=*/false, /*fresh=*/false);
                if (entry.IsSpent()) {
                    assert(memusage::DynamicUsage(entry.script) == 0);
                    continue;
                }
            }
            if (keep != i) shard.entries[keep] = std::move(entry);
            ++keep;
        }
        const bool shrunk{keep < shard.entries.size()};
        shard.entries.resize(keep);
        shard.dirty_count = 0;
        if (shrunk) {
            if (shard.entries.empty()) {
                shard.index.clear();
            } else {
                Rehash(shard, std::max<size_t>(16, shard.entries.size() * 10 / 7 + 1));
            }
        }
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
        size_t dirty{0};
        for (uint32_t i{0}; i < shard.entries.size(); ++i) {
            const PackedEntry& entry{shard.entries[i]};
            usage += memusage::DynamicUsage(entry.script);
            if (entry.IsDirty()) {
                ++dirty;
            } else {
                assert(!entry.IsFresh());
                assert(!entry.IsSpent());
            }
            if (entry.IsFresh()) {
                assert(!entry.IsSpent());
            }
            assert(FindEntry(shard, entry.key, HashKey(entry.key)) == i);
        }
        assert(dirty == shard.dirty_count);
        assert(usage == shard.script_bytes);
        const size_t shard_bytes{memusage::DynamicUsage(shard.entries) + memusage::DynamicUsage(shard.index) + usage};
        assert(shard.a_bytes.load(std::memory_order_relaxed) == shard_bytes);
        assert(shard.a_entries.load(std::memory_order_relaxed) == shard.entries.size());
        assert(shard.a_dirty.load(std::memory_order_relaxed) == dirty);
        total_bytes += shard_bytes;
        total_entries += shard.entries.size();
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
