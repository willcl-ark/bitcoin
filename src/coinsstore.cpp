// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <coinsstore.h>

#include <crypto/common.h>
#include <logging.h>
#include <memusage.h>
#include <util/check.h>
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

size_t WindowOverlay::FlatTable::FindIndex(const Txid& txid, uint64_t h) const noexcept
{
    if (size == 0) return NOT_FOUND;
    const size_t mask{ctrl.size() - 1};
    const uint8_t tag{Tag(h)};
    for (size_t pos{static_cast<size_t>(h) & mask};; pos = (pos + 1) & mask) {
        const uint8_t c{ctrl[pos]};
        if (c == EMPTY) return NOT_FOUND;
        if (c == tag && slots[pos].hash == h && slots[pos].tx->GetHash() == txid) return pos;
    }
}

WindowOverlay::TxEntry* WindowOverlay::FlatTable::Find(const Txid& txid, uint64_t h) noexcept
{
    const size_t pos{FindIndex(txid, h)};
    return pos == NOT_FOUND ? nullptr : &slots[pos];
}

const WindowOverlay::TxEntry* WindowOverlay::FlatTable::Find(const Txid& txid, uint64_t h) const noexcept
{
    const size_t pos{FindIndex(txid, h)};
    return pos == NOT_FOUND ? nullptr : &slots[pos];
}

WindowOverlay::TxEntry* WindowOverlay::FlatTable::TryInsert(const TxEntry& entry)
{
    if (ctrl.empty() || (size + tombstones + 1) * 4 >= ctrl.size() * 3) {
        size_t target{std::max<size_t>(16, std::bit_ceil((size + 1) * 2))};
        if (!ctrl.empty() && target <= ctrl.size() && size * 2 >= ctrl.size()) target = ctrl.size() * 2;
        Rehash(target);
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

void WindowOverlay::FlatTable::EraseAt(size_t idx) noexcept
{
    const size_t mask{ctrl.size() - 1};
    slots[idx].tx = nullptr;
    --size;
    if (ctrl[(idx + 1) & mask] == EMPTY) {
        ctrl[idx] = EMPTY;
    } else {
        ctrl[idx] = TOMBSTONE;
        ++tombstones;
    }
}

bool WindowOverlay::FlatTable::Erase(const Txid& txid, uint64_t h, int32_t height) noexcept
{
    const size_t pos{FindIndex(txid, h)};
    if (pos == NOT_FOUND || slots[pos].height != height) return false;
    EraseAt(pos);
    return true;
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

    const size_t n_tx{block->vtx.size()};
    thread_local std::vector<TxEntry> entries;
    thread_local std::vector<uint32_t> shard_of;
    thread_local std::vector<uint32_t> tx_index;
    thread_local std::vector<uint32_t> order;
    thread_local std::vector<std::pair<COutPoint, uint32_t>> high_spends;

    entries.resize(n_tx);
    shard_of.resize(n_tx);
    high_spends.clear();

    const size_t idx_cap{n_tx > 1 ? std::bit_ceil(n_tx * 2) : 0};
    tx_index.assign(idx_cap, std::numeric_limits<uint32_t>::max());
    const size_t idx_mask{idx_cap ? idx_cap - 1 : 0};

    std::array<uint32_t, NUM_SHARDS + 1> starts{};
    for (size_t i{0}; i < n_tx; ++i) {
        const CTransaction* tx{block->vtx[i].get()};
        const Txid& txid{tx->GetHash()};
        const uint64_t h{TxHash(txid)};
        const uint32_t s{static_cast<uint32_t>(ShardIndex(txid))};
        entries[i] = TxEntry{
            .tx = tx,
            .intra_spent_mask = 0,
            .hash = h,
            .height = height,
            .is_coinbase = (i == 0),
            .has_high_intra_spend = false,
        };
        shard_of[i] = s;
        ++starts[s + 1];
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

    for (size_t s{0}; s < NUM_SHARDS; ++s) starts[s + 1] += starts[s];
    order.resize(n_tx);
    std::array<uint32_t, NUM_SHARDS> next;
    std::copy_n(starts.begin(), NUM_SHARDS, next.begin());
    for (uint32_t i{0}; i < n_tx; ++i) order[next[shard_of[i]]++] = i;

    BlockMeta meta;
    meta.block = block;
    meta.starts = starts;
    meta.items.resize(n_tx);
    for (size_t k{0}; k < n_tx; ++k) {
        const TxEntry& e{entries[order[k]]};
        meta.items[k] = RemItem{&e.tx->GetHash(), e.hash};
    }

    {
        LOCK(m_blocks_mutex);
        m_blocks.insert_or_assign(height, std::move(meta));
        m_total_blocks.store(m_blocks.size(), std::memory_order_release);
    }

    const size_t start_offset{static_cast<size_t>(entries[0].hash) & (NUM_SHARDS - 1)};
    for (size_t si{0}; si < NUM_SHARDS; ++si) {
        const size_t s{(start_offset + si) & (NUM_SHARDS - 1)};
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

void WindowOverlay::EraseDetached(std::vector<std::pair<int, BlockMeta>>& removing, ThreadPool* pool)
{
    if (removing.empty()) return;
    const auto remove_shard{[&](size_t s) noexcept {
        bool any{false};
        for (const auto& [h, meta] : removing) {
            if (meta.starts[s] < meta.starts[s + 1]) {
                any = true;
                break;
            }
        }
        if (!any) return;
        Shard& shard{m_shards[s]};
        LOCK(shard.mutex);
        for (const auto& [h, meta] : removing) {
            const uint32_t begin{meta.starts[s]};
            const uint32_t end{meta.starts[s + 1]};
            for (uint32_t k{begin}; k < end; ++k) {
                const RemItem& item{meta.items[k]};
                if (!shard.dup_txs.empty()) {
                    auto dit{std::find_if(shard.dup_txs.begin(), shard.dup_txs.end(), [&](const TxEntry& e) {
                        return e.height == h && e.hash == item.hash && e.tx->GetHash() == *item.txid;
                    })};
                    if (dit != shard.dup_txs.end()) {
                        *dit = shard.dup_txs.back();
                        shard.dup_txs.pop_back();
                        continue;
                    }
                }
                if (const size_t idx{shard.txs.FindIndex(*item.txid, item.hash)}; idx != FlatTable::NOT_FOUND && shard.txs.slots[idx].height == h) {
                    if (!shard.dup_txs.empty()) {
                        auto best{shard.dup_txs.end()};
                        for (auto dit{shard.dup_txs.begin()}; dit != shard.dup_txs.end(); ++dit) {
                            if (dit->hash == item.hash && dit->tx->GetHash() == *item.txid &&
                                (best == shard.dup_txs.end() || dit->height < best->height)) {
                                best = dit;
                            }
                        }
                        if (best != shard.dup_txs.end()) {
                            shard.txs.slots[idx] = *best;
                            *best = shard.dup_txs.back();
                            shard.dup_txs.pop_back();
                            continue;
                        }
                    }
                    shard.txs.EraseAt(idx);
                }
            }
        }
        if (!shard.high_intra_spends.empty()) {
            std::erase_if(shard.high_intra_spends, [&](const auto& p) {
                for (const auto& [h, meta] : removing) {
                    if (p.second == h) return true;
                }
                return false;
            });
        }
    }};

    size_t total_items{0};
    for (const auto& [h, meta] : removing) total_items += meta.items.size();
    const size_t helpers{pool && total_items >= 512 ? std::min<size_t>(pool->WorkersCount(), 8) : 0};
    if (helpers > 0) {
        struct Shared {
            std::atomic<size_t> next{0};
            std::atomic<int> active{0};
        };
        const auto shared{std::make_shared<Shared>()};
        for (size_t hi{0}; hi < helpers; ++hi) {
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

void WindowOverlay::RemoveBlocks(std::span<const int> heights, ThreadPool* pool)
{
    if (heights.empty()) return;
    std::vector<std::pair<int, BlockMeta>> removing;
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
    EraseDetached(removing, pool);
}

WindowOverlay::~WindowOverlay()
{
    WaitForRemovals();
}

void WindowOverlay::RemoveBlocksAsync(std::span<const int> heights, ThreadPool* pool)
{
    if (heights.empty()) return;
    auto removing{std::make_shared<std::vector<std::pair<int, BlockMeta>>>()};
    removing->reserve(heights.size());
    size_t total_items{0};
    {
        LOCK(m_blocks_mutex);
        for (const int h : heights) {
            if (auto it{m_blocks.find(h)}; it != m_blocks.end()) {
                total_items += it->second.items.size();
                removing->emplace_back(h, std::move(it->second));
                m_blocks.erase(it);
            }
        }
        m_total_blocks.store(m_blocks.size(), std::memory_order_release);
    }
    if (removing->empty()) return;
    if (total_items < 64 || !pool || pool->WorkersCount() == 0) {
        EraseDetached(*removing, nullptr);
        return;
    }
    {
        LOCK(m_remove_mutex);
        ++m_pending_removes;
    }
    auto task{[this, removing]() noexcept {
        WAITED_FOR_UNDER_LOCK("a window overlay async removal");
        EraseDetached(*removing, nullptr);
        removing->clear();
        LOCK(m_remove_mutex);
        if (--m_pending_removes == 0) m_remove_cv.notify_all();
    }};
    if (!pool->Submit(std::move(task))) {
        EraseDetached(*removing, nullptr);
        removing->clear();
        LOCK(m_remove_mutex);
        if (--m_pending_removes == 0) m_remove_cv.notify_all();
    }
}

void WindowOverlay::WaitForRemovals()
{
    WAIT_LOCK(m_remove_mutex, lock);
    m_remove_cv.wait(lock, [this]() EXCLUSIVE_LOCKS_REQUIRED(m_remove_mutex) { return m_pending_removes == 0; });
}

void WindowOverlay::Clear()
{
    WaitForRemovals();
    std::unordered_map<int, BlockMeta> blocks;
    {
        LOCK(m_blocks_mutex);
        blocks.swap(m_blocks);
        m_total_blocks.store(0, std::memory_order_release);
    }
    m_committed_height.store(-1, std::memory_order_release);
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
    const int min_height{m_committed_height.load(std::memory_order_acquire)};
    const uint64_t h{TxHash(outpoint.hash)};
    const Shard& shard{m_shards[ShardIndex(outpoint.hash)]};
    LOCK(shard.mutex);
    const TxEntry* entry{shard.txs.Find(outpoint.hash, h)};
    if (!entry) return std::nullopt;
    const TxEntry* best{(entry->height < viewer_height && entry->height > min_height) ? entry : nullptr};
    if (!shard.dup_txs.empty()) {
        for (const TxEntry& d : shard.dup_txs) {
            if (d.hash == h && d.tx->GetHash() == outpoint.hash && d.height < viewer_height && d.height > min_height &&
                (!best || d.height > best->height)) {
                best = &d;
            }
        }
    }
    if (!best) return std::nullopt;
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
    const int min_height{m_committed_height.load(std::memory_order_acquire)};
    thread_local std::vector<uint32_t> shard_of;
    thread_local std::vector<uint64_t> hashes;
    thread_local std::vector<uint32_t> order;
    shard_of.resize(outpoints.size());
    hashes.resize(outpoints.size());
    order.resize(outpoints.size());
    std::array<uint32_t, NUM_SHARDS + 1> starts{};
    for (size_t i{0}; i < outpoints.size(); ++i) {
        const COutPoint& op{outpoints[i]};
        const uint32_t s{static_cast<uint32_t>(ShardIndex(op.hash))};
        shard_of[i] = s;
        hashes[i] = TxHash(op.hash);
        ++starts[s + 1];
    }
    for (size_t s{0}; s < NUM_SHARDS; ++s) starts[s + 1] += starts[s];
    std::array<uint32_t, NUM_SHARDS> next;
    std::copy_n(starts.begin(), NUM_SHARDS, next.begin());
    for (uint32_t i{0}; i < outpoints.size(); ++i) order[next[shard_of[i]]++] = i;

    const size_t start_offset{static_cast<size_t>(hashes[0]) & (NUM_SHARDS - 1)};
    for (size_t si{0}; si < NUM_SHARDS; ++si) {
        const size_t s{(start_offset + si) & (NUM_SHARDS - 1)};
        if (starts[s] == starts[s + 1]) continue;
        const Shard& shard{m_shards[s]};
        LOCK(shard.mutex);
        if (shard.txs.empty()) {
            for (uint32_t k{starts[s]}; k < starts[s + 1]; ++k) misses.push_back(outpoints[order[k]]);
            continue;
        }
        const uint32_t s_begin{starts[s]};
        const uint32_t s_end{starts[s + 1]};
        for (uint32_t k{s_begin}; k < s_end; ++k) {
            const uint32_t idx{order[k]};
            const COutPoint& op{outpoints[idx]};
            const uint64_t h{hashes[idx]};
            const TxEntry* entry{shard.txs.Find(op.hash, h)};
            if (!entry) {
                misses.push_back(op);
                continue;
            }
            const TxEntry* best{(entry->height < viewer_height && entry->height > min_height) ? entry : nullptr};
            if (!shard.dup_txs.empty()) {
                for (const TxEntry& d : shard.dup_txs) {
                    if (d.hash == h && d.tx->GetHash() == op.hash && d.height < viewer_height && d.height > min_height &&
                        (!best || d.height > best->height)) {
                        best = &d;
                    }
                }
            }
            if (!best) {
                misses.push_back(op);
                continue;
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
// CoinsStore::FlatMap (SwissTable-style flat open-addressed table on mmap)

void* CoinsStore::FlatMap::AllocRegion(size_t bytes, bool& mmapped)
{
#ifndef WIN32
    if (bytes >= HUGE_PAGE_BYTES) {
        void* p{::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0)};
        if (p != MAP_FAILED) {
#ifdef MADV_HUGEPAGE
            if (bytes >= HUGE_PAGE_BYTES * 4) ::madvise(p, bytes, MADV_HUGEPAGE);
#endif
            mmapped = true;
            return p;
        }
    }
#endif
    mmapped = false;
    return ::operator new(bytes, std::align_val_t{64});
}

void CoinsStore::FlatMap::FreeRegion(void* ptr, size_t bytes, bool mmapped) noexcept
{
    if (!ptr) return;
#ifndef WIN32
    if (mmapped) {
        ::munmap(ptr, bytes);
        return;
    }
#endif
    ::operator delete(ptr, std::align_val_t{64});
}

void CoinsStore::FlatMap::Clear() noexcept
{
    if (m_cap != 0) {
        for (size_t i{0}; i < m_cap; ++i) {
            if (IsOccupiedAt(i)) m_slots[i].~Slot();
        }
        FreeRegion(m_region, m_region_bytes, m_mmapped);
    }
    m_ctrl = nullptr;
    m_slots = nullptr;
    m_region = nullptr;
    m_region_bytes = 0;
    m_cap = 0;
    m_size = 0;
    m_tombstones = 0;
    m_mmapped = false;
    ++m_gen;
}

// Portable borrow-free SWAR 8-byte control group matching (no arch-specific intrinsics).
// Returns a mask with bit (8*i + 7) set iff byte i of `word` equals `needle`.
static inline uint64_t MatchBytes8(uint64_t word, uint8_t needle) noexcept
{
    constexpr uint64_t ONES{0x0101010101010101ULL};
    constexpr uint64_t LO7{0x7F7F7F7F7F7F7F7FULL};
    const uint64_t x{word ^ (ONES * needle)};
    return ~(((x & LO7) + LO7) | x | LO7);
}

size_t CoinsStore::FlatMap::Find(const COutPoint& key, uint64_t hash) const noexcept
{
    if (m_size == 0) return NOT_FOUND;
    const size_t mask{m_cap - 1};
    const uint8_t tag{Tag(hash)};
    size_t pos{(static_cast<size_t>(hash) & mask) & ~size_t{7}};
    while (true) {
        const uint64_t group{ReadLE64(m_ctrl + pos)};
        uint64_t match{MatchBytes8(group, tag)};
        while (match) {
            const size_t idx{pos + (static_cast<unsigned>(std::countr_zero(match)) >> 3)};
            if (m_slots[idx].key == key) return idx;
            match &= match - 1;
        }
        if (MatchBytes8(group, EMPTY) != 0) return NOT_FOUND;
        pos = (pos + 8) & mask;
    }
}

void CoinsStore::FlatMap::Rehash(size_t new_cap)
{
    static_assert(std::is_nothrow_move_constructible_v<Slot>);
    if (new_cap == 0) {
        Clear();
        return;
    }
    new_cap = std::max<size_t>(MIN_CAP, std::bit_ceil(new_cap));
    const size_t ctrl_bytes{AlignUp64(new_cap)};
    const size_t total_bytes{ctrl_bytes + new_cap * sizeof(Slot)};
    bool new_mmapped{false};
    void* new_region{AllocRegion(total_bytes, new_mmapped)};
    auto* new_ctrl{static_cast<uint8_t*>(new_region)};
    if (!new_mmapped) std::memset(new_ctrl, EMPTY, new_cap);
    auto* new_slots{reinterpret_cast<Slot*>(new_ctrl + ctrl_bytes)};
    const size_t new_mask{new_cap - 1};

    for (size_t i{0}; i < m_cap; ++i) {
        if (!IsOccupiedAt(i)) continue;
        const uint64_t h{KeyHash(m_slots[i].key)};
        const uint8_t tag{Tag(h)};
        size_t pos{(static_cast<size_t>(h) & new_mask) & ~size_t{7}};
        size_t dst;
        while (true) {
            const uint64_t group{ReadLE64(new_ctrl + pos)};
            const uint64_t empties{MatchBytes8(group, EMPTY)};
            if (empties) {
                dst = pos + (static_cast<unsigned>(std::countr_zero(empties)) >> 3);
                break;
            }
            pos = (pos + 8) & new_mask;
        }
        new_ctrl[dst] = tag;
        new (&new_slots[dst]) Slot(std::move(m_slots[i]));
        m_slots[i].~Slot();
    }

    FreeRegion(m_region, m_region_bytes, m_mmapped);
    m_region = new_region;
    m_region_bytes = total_bytes;
    m_ctrl = new_ctrl;
    m_slots = new_slots;
    m_cap = new_cap;
    m_tombstones = 0;
    m_mmapped = new_mmapped;
    ++m_gen;
}

void CoinsStore::FlatMap::EnsureCapacity(size_t additional)
{
    if (!NeedsGrow(additional)) return;
    const size_t needed{m_size + additional};
    size_t target{std::max<size_t>(MIN_CAP, std::bit_ceil((needed * 8) / 7 + 1))};
    // Avoid thrashing same-capacity tombstone rehashes when live load is already high (> 62.5%).
    if (m_cap != 0 && target <= m_cap && needed * 8 >= m_cap * 5) {
        target = m_cap * 2;
    }
    Rehash(target);
}

std::pair<size_t, bool> CoinsStore::FlatMap::TryEmplace(const COutPoint& key, uint64_t hash)
{
    if (NeedsGrow(1)) {
        if (m_cap == 0) {
            Rehash(MIN_CAP);
        } else if (m_size * 8 < m_cap * 5) {
            Rehash(m_cap);
        } else {
            Rehash(m_cap * 2);
        }
    }
    const size_t mask{m_cap - 1};
    const uint8_t tag{Tag(hash)};
    size_t pos{(static_cast<size_t>(hash) & mask) & ~size_t{7}};
    size_t first_tomb{NOT_FOUND};

    while (true) {
        const uint64_t group{ReadLE64(m_ctrl + pos)};
        uint64_t match{MatchBytes8(group, tag)};
        while (match) {
            const size_t idx{pos + (static_cast<unsigned>(std::countr_zero(match)) >> 3)};
            if (m_slots[idx].key == key) return {idx, false};
            match &= match - 1;
        }
        if (first_tomb == NOT_FOUND) {
            const uint64_t tombs{MatchBytes8(group, TOMBSTONE)};
            if (tombs) first_tomb = pos + (static_cast<unsigned>(std::countr_zero(tombs)) >> 3);
        }
        const uint64_t empties{MatchBytes8(group, EMPTY)};
        if (empties) {
            const size_t dst{first_tomb != NOT_FOUND ? first_tomb : (pos + (static_cast<unsigned>(std::countr_zero(empties)) >> 3))};
            if (first_tomb != NOT_FOUND) --m_tombstones;
            SetCtrl(dst, tag);
            new (&m_slots[dst]) Slot{};
            m_slots[dst].key = key;
            ++m_size;
            return {dst, true};
        }
        pos = (pos + 8) & mask;
    }
}

void CoinsStore::FlatMap::EraseAt(size_t idx) noexcept
{
    Assert(idx < m_cap && IsOccupiedAt(idx));
    m_slots[idx].~Slot();
    const size_t group_start{idx & ~size_t{7}};
    const bool group_has_empty{MatchBytes8(ReadLE64(m_ctrl + group_start), EMPTY) != 0};
    if (group_has_empty) {
        SetCtrl(idx, EMPTY);
    } else {
        SetCtrl(idx, TOMBSTONE);
        ++m_tombstones;
    }
    --m_size;
}

void CoinsStore::FlatMap::ShrinkToFit()
{
    if (m_size == 0) {
        Clear();
        return;
    }
    const size_t target{std::max<size_t>(MIN_CAP, std::bit_ceil((m_size * 8) / 7 + 1))};
    if (target < m_cap) Rehash(target);
}

// ---------------------------------------------------------------------------------------------------------
// CoinsStore lifecycle & accounting

CoinsStore::CoinsStore(CCoinsView* base_view, size_t shards, bool /*deterministic*/)
    : CCoinsViewCache(base_view, /*deterministic=*/true),
      m_shards(std::max<size_t>(shards, 1))
{
    m_tier_block_hash = base->GetBestBlock();
}

CoinsStore::~CoinsStore() = default;

CoinsStore::Totals& CoinsStore::MyTotals() const
{
    static std::atomic<size_t> next_stripe{0};
    thread_local const size_t stripe{next_stripe.fetch_add(1, std::memory_order_relaxed) % TOTALS_STRIPES};
    return m_totals[stripe];
}

size_t CoinsStore::SumTotals(std::atomic<int64_t> Totals::* field) const
{
    int64_t sum{0};
    for (const Totals& stripe : m_totals) sum += (stripe.*field).load(std::memory_order_relaxed);
    return sum > 0 ? static_cast<size_t>(sum) : 0;
}

size_t CoinsStore::SpanOf(int height)
{
    return static_cast<size_t>(std::max(height, 0)) >> DIRTY_SPAN_BITS;
}

int64_t CoinsStore::DirtyBytes(const Slot& slot) noexcept
{
    return static_cast<int64_t>(sizeof(Slot) + 1 + slot.coin.DynamicMemoryUsage());
}

void CoinsStore::AddDirtySpanBytes(Shard& shard, int height, int64_t delta) const
{
    AssertLockHeld(shard.mutex);
    const size_t span{SpanOf(height)};
    if (delta > 0 && span >= shard.dirty_span_bytes.size()) shard.dirty_span_bytes.resize(span + 1, 0);
    if (span < shard.dirty_span_bytes.size()) shard.dirty_span_bytes[span] += delta;
}

void CoinsStore::AddUsage(Shard& shard, size_t bytes) const
{
    AssertLockHeld(shard.mutex);
    if (bytes == 0) return;
    shard.heap_bytes += bytes;
    MyTotals().bytes.fetch_add(static_cast<int64_t>(bytes), std::memory_order_relaxed);
}

void CoinsStore::SubUsage(Shard& shard, size_t bytes) const
{
    AssertLockHeld(shard.mutex);
    if (bytes == 0) return;
    shard.heap_bytes -= bytes;
    MyTotals().bytes.fetch_sub(static_cast<int64_t>(bytes), std::memory_order_relaxed);
}

void CoinsStore::SyncTableStats(Shard& shard) const
{
    AssertLockHeld(shard.mutex);
    const size_t entries{shard.table.size()};
    const size_t table_bytes{shard.table.MemoryBytes()};
    if (entries != shard.entries) {
        MyTotals().entries.fetch_add(static_cast<int64_t>(entries) - static_cast<int64_t>(shard.entries), std::memory_order_relaxed);
        shard.entries = entries;
    }
    if (table_bytes != shard.table_bytes) {
        MyTotals().bytes.fetch_add(static_cast<int64_t>(table_bytes) - static_cast<int64_t>(shard.table_bytes), std::memory_order_relaxed);
        shard.table_bytes = table_bytes;
    }
}

void CoinsStore::SyncDirtyCount(Shard& shard, int64_t delta) const
{
    AssertLockHeld(shard.mutex);
    if (delta == 0) return;
    shard.dirty_count = static_cast<size_t>(static_cast<int64_t>(shard.dirty_count) + delta);
    MyTotals().dirty.fetch_add(delta, std::memory_order_relaxed);
    if (shard.dirty_count == 0) shard.dirty_span_bytes.clear();
}

void CoinsStore::EraseSlot(Shard& shard, size_t pos) const
{
    AssertLockHeld(shard.mutex);
    Slot& slot{shard.table.SlotAt(pos)};
    if (slot.dirty) {
        AddDirtySpanBytes(shard, slot.height, -DirtyBytes(slot));
        SyncDirtyCount(shard, -1);
    }
    SubUsage(shard, slot.coin.DynamicMemoryUsage());
    shard.table.EraseAt(pos);
    SyncTableStats(shard);
}

// ---------------------------------------------------------------------------------------------------------
// CCoinsView / CCoinsViewCache confirmed-state interface

std::optional<Coin> CoinsStore::Lookup(const COutPoint& outpoint) const
{
    Shard& shard{ShardFor(outpoint)};
    const uint64_t h{KeyHash(outpoint)};
    while (true) {
        uint64_t settles;
        {
            LOCK(shard.mutex);
            if (const size_t pos{shard.table.Find(outpoint, h)}; pos != NOT_FOUND) {
                Slot& slot{shard.table.SlotAt(pos)};
                if (slot.coin.IsSpent()) return std::nullopt;
                Reference(slot);
                return slot.coin.ToCoin();
            }
            settles = shard.settles;
        }
        std::optional<Coin> coin{base->GetCoin(outpoint)};
        LOCK(shard.mutex);
        if (shard.settles != settles) continue;
        if (const size_t pos{shard.table.Find(outpoint, h)}; pos != NOT_FOUND) {
            Slot& slot{shard.table.SlotAt(pos)};
            if (slot.coin.IsSpent()) return std::nullopt;
            Reference(slot);
            return slot.coin.ToCoin();
        }
        if (!coin) return std::nullopt;
        Assert(!coin->IsSpent());
        const size_t pos{shard.table.TryEmplace(outpoint, h).first};
        Slot& slot{shard.table.SlotAt(pos)};
        slot.coin = *coin;
        Transit(slot, coins_entry::ABSENT, Event::BaseCopy, static_cast<int>(coin->nHeight));
        AddUsage(shard, slot.coin.DynamicMemoryUsage());
        SyncTableStats(shard);
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
    const uint64_t h{KeyHash(outpoint)};
    while (true) {
        uint64_t settles;
        {
            LOCK(shard.mutex);
            if (const size_t pos{shard.table.Find(outpoint, h)}; pos != NOT_FOUND) {
                Slot& slot{shard.table.SlotAt(pos)};
                if (slot.coin.IsSpent()) return std::nullopt;
                Reference(slot);
                return slot.coin.ToCoin();
            }
            settles = shard.settles;
        }
        std::optional<Coin> coin{base->PeekCoin(outpoint)};
        if (!coin) return std::nullopt;
        LOCK(shard.mutex);
        if (shard.settles != settles) continue;
        if (const size_t pos{shard.table.Find(outpoint, h)}; pos != NOT_FOUND) {
            Slot& slot{shard.table.SlotAt(pos)};
            if (slot.coin.IsSpent()) return std::nullopt;
            Reference(slot);
            return slot.coin.ToCoin();
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
    const Shard& shard{ShardFor(outpoint)};
    LOCK(shard.mutex);
    const size_t pos{shard.table.Find(outpoint)};
    return pos != NOT_FOUND && !shard.table.SlotAt(pos).coin.IsSpent();
}

const Coin& CoinsStore::AccessCoin(const COutPoint& outpoint) const
{
    static const Coin coin_empty;
    thread_local Coin returned;
    if (std::optional<Coin> coin{Lookup(outpoint)}) {
        returned = std::move(*coin);
        return returned;
    }
    return coin_empty;
}

uint256 CoinsStore::GetBestBlock() const
{
    if (m_tier_block_hash.IsNull()) m_tier_block_hash = base->GetBestBlock();
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

void CoinsStore::AddCoin(const COutPoint& outpoint, Coin&& coin, bool possible_overwrite)
{
    Assert(!coin.IsSpent());
    if (coin.out.scriptPubKey.IsUnspendable()) return;
    Shard& shard{ShardFor(outpoint)};
    const uint64_t h{KeyHash(outpoint)};
    LOCK(shard.mutex);
    auto [pos, inserted]{shard.table.TryEmplace(outpoint, h)};
    Slot& slot{shard.table.SlotAt(pos)};
    const State from{inserted ? coins_entry::ABSENT : StateOf(slot)};
    if (!inserted && !possible_overwrite && !slot.coin.IsSpent()) {
        throw std::logic_error("Attempted to overwrite an unspent coin (when possible_overwrite is false)");
    }
    const bool was_dirty{!inserted && slot.dirty};
    if (was_dirty) AddDirtySpanBytes(shard, slot.height, -DirtyBytes(slot));
    SubUsage(shard, slot.coin.DynamicMemoryUsage());
    const int tag{TagFor(coin)};
    slot.coin = std::move(coin);
    if (inserted) {
        Transit(slot, from, possible_overwrite ? Event::ConfirmedWrite : Event::ConfirmedNew, tag);
    } else {
        Transit(slot, from, Event::ConfirmedWrite, tag);
    }
    AddUsage(shard, slot.coin.DynamicMemoryUsage());
    AddDirtySpanBytes(shard, slot.height, DirtyBytes(slot));
    if (!was_dirty) SyncDirtyCount(shard, 1);
    SyncTableStats(shard);
}

void CoinsStore::EmplaceCoinInternalDANGER(const COutPoint& outpoint, Coin&& coin)
{
    Shard& shard{ShardFor(outpoint)};
    const uint64_t h{KeyHash(outpoint)};
    LOCK(shard.mutex);
    auto [pos, inserted]{shard.table.TryEmplace(outpoint, h)};
    if (!inserted) return;
    Slot& slot{shard.table.SlotAt(pos)};
    slot.coin = std::move(coin);
    Transit(slot, coins_entry::ABSENT, slot.coin.IsSpent() ? Event::ConfirmedSpend : Event::ConfirmedWrite, m_change_height);
    AddUsage(shard, slot.coin.DynamicMemoryUsage());
    AddDirtySpanBytes(shard, slot.height, DirtyBytes(slot));
    SyncDirtyCount(shard, 1);
    SyncTableStats(shard);
}

bool CoinsStore::SpendSlot(Shard& shard, size_t pos, Coin* moveto)
{
    AssertLockHeld(shard.mutex);
    Slot& slot{shard.table.SlotAt(pos)};
    if (slot.coin.IsSpent()) {
        if (moveto) *moveto = Coin{};
        return true;
    }
    const State from{StateOf(slot)};
    const bool was_dirty{slot.dirty != 0};
    if (moveto) *moveto = slot.coin.ToCoin();
    if (slot.fresh) {
        EraseSlot(shard, pos);
        return true;
    }
    if (was_dirty) AddDirtySpanBytes(shard, slot.height, -DirtyBytes(slot));
    SubUsage(shard, slot.coin.DynamicMemoryUsage());
    slot.coin.Clear();
    Transit(slot, from, Event::ConfirmedSpend, m_change_height);
    AddDirtySpanBytes(shard, slot.height, DirtyBytes(slot));
    if (!was_dirty) SyncDirtyCount(shard, 1);
    return true;
}

bool CoinsStore::SpendCoin(const COutPoint& outpoint, Coin* moveto)
{
    Shard& shard{ShardFor(outpoint)};
    const uint64_t h{KeyHash(outpoint)};
    while (true) {
        uint64_t settles;
        {
            LOCK(shard.mutex);
            if (const size_t pos{shard.table.Find(outpoint, h)}; pos != NOT_FOUND) return SpendSlot(shard, pos, moveto);
            settles = shard.settles;
        }
        std::optional<Coin> coin{base->GetCoin(outpoint)};
        LOCK(shard.mutex);
        if (shard.settles != settles) continue;
        if (const size_t pos{shard.table.Find(outpoint, h)}; pos != NOT_FOUND) return SpendSlot(shard, pos, moveto);
        if (!coin) return false;
        if (moveto) *moveto = std::move(*coin);
        const size_t pos{shard.table.TryEmplace(outpoint, h).first};
        Slot& slot{shard.table.SlotAt(pos)};
        Transit(slot, coins_entry::ABSENT, Event::ConfirmedSpend, m_change_height);
        AddDirtySpanBytes(shard, slot.height, DirtyBytes(slot));
        SyncDirtyCount(shard, 1);
        SyncTableStats(shard);
        return true;
    }
}

void CoinsStore::Uncache(const COutPoint& outpoint)
{
    Shard& shard{ShardFor(outpoint)};
    LOCK(shard.mutex);
    const size_t pos{shard.table.Find(outpoint)};
    if (pos == NOT_FOUND || shard.table.SlotAt(pos).dirty) return;
    EraseSlot(shard, pos);
}

void CoinsStore::BatchWrite(CoinsViewCacheCursor& cursor, const uint256& block_hash)
{
    for (auto it{cursor.Begin()}; it != cursor.End();) {
        if (!it->second.IsDirty()) {
            it = cursor.NextAndMaybeErase(*it);
            continue;
        }
        const COutPoint& outpoint{it->first};
        const bool child_fresh{it->second.IsFresh()};
        Coin& child_coin{it->second.coin};
        Shard& shard{ShardFor(outpoint)};
        const uint64_t h{KeyHash(outpoint)};
        {
            LOCK(shard.mutex);
            const size_t existing{shard.table.Find(outpoint, h)};
            if (existing == NOT_FOUND) {
                if (!child_coin.IsSpent()) {
                    const size_t pos{shard.table.TryEmplace(outpoint, h).first};
                    Slot& slot{shard.table.SlotAt(pos)};
                    const int tag{TagFor(child_coin)};
                    slot.coin = std::move(child_coin);
                    Transit(slot, coins_entry::ABSENT, child_fresh ? Event::ConfirmedNew : Event::ConfirmedWrite, tag);
                    AddUsage(shard, slot.coin.DynamicMemoryUsage());
                    AddDirtySpanBytes(shard, slot.height, DirtyBytes(slot));
                    SyncDirtyCount(shard, 1);
                    SyncTableStats(shard);
                } else if (!child_fresh) {
                    const size_t pos{shard.table.TryEmplace(outpoint, h).first};
                    Slot& slot{shard.table.SlotAt(pos)};
                    Transit(slot, coins_entry::ABSENT, Event::ConfirmedSpend, m_change_height);
                    AddDirtySpanBytes(shard, slot.height, DirtyBytes(slot));
                    SyncDirtyCount(shard, 1);
                    SyncTableStats(shard);
                }
            } else {
                Slot& slot{shard.table.SlotAt(existing)};
                const State from{StateOf(slot)};
                const bool was_dirty{slot.dirty != 0};
                if (child_fresh && !slot.coin.IsSpent()) {
                    throw std::logic_error("FRESH flag misapplied to coin that exists in parent cache");
                }
                if (slot.fresh && child_coin.IsSpent()) {
                    EraseSlot(shard, existing);
                } else if (child_coin.IsSpent()) {
                    if (!slot.coin.IsSpent()) {
                        if (was_dirty) AddDirtySpanBytes(shard, slot.height, -DirtyBytes(slot));
                        SubUsage(shard, slot.coin.DynamicMemoryUsage());
                        slot.coin.Clear();
                        Transit(slot, from, Event::ConfirmedSpend, m_change_height);
                        AddDirtySpanBytes(shard, slot.height, DirtyBytes(slot));
                        if (!was_dirty) SyncDirtyCount(shard, 1);
                    }
                } else {
                    if (was_dirty) AddDirtySpanBytes(shard, slot.height, -DirtyBytes(slot));
                    SubUsage(shard, slot.coin.DynamicMemoryUsage());
                    const int tag{TagFor(child_coin)};
                    slot.coin = std::move(child_coin);
                    Transit(slot, from, Event::BatchOverwrite, tag);
                    AddUsage(shard, slot.coin.DynamicMemoryUsage());
                    AddDirtySpanBytes(shard, slot.height, DirtyBytes(slot));
                    if (!was_dirty) SyncDirtyCount(shard, 1);
                }
            }
        }
        it = cursor.NextAndMaybeErase(*it);
    }
    if (!block_hash.IsNull()) m_tier_block_hash = block_hash;
}

void CoinsStore::Reset() noexcept
{
    for (Shard& shard : m_shards) {
        LOCK(shard.mutex);
        shard.table.Clear();
        shard.dirty_span_bytes.clear();
        shard.revert_meta.clear();
        shard.revert_coins.clear();
        shard.revert_batch_id = 0;
        shard.dirty_count = 0;
        shard.entries = 0;
        shard.table_bytes = 0;
        shard.heap_bytes = 0;
        shard.clock_hand = 0;
    }
    for (Totals& stripe : m_totals) {
        stripe.bytes.store(0, std::memory_order_relaxed);
        stripe.entries.store(0, std::memory_order_relaxed);
        stripe.dirty.store(0, std::memory_order_relaxed);
    }
    m_tier_block_hash.SetNull();
    m_tier_height = -1;
    m_confirmed_height.store(-1, std::memory_order_release);
    m_write_out_epoch.fetch_add(1, std::memory_order_release);
    m_restored.clear();
}

// ---------------------------------------------------------------------------------------------------------
// Shard-batched read-only FetchConfirmed & shard-parallel CheckBatchConflicts / ApplyCheckedBlocks / CommitBatch

template <typename Work>
void CoinsStore::VisitShards(size_t start_hint, const std::vector<uint32_t>& shard_of, Work work) const
{
    const size_t num_shards{m_shards.size()};
    thread_local std::vector<uint32_t> starts;
    thread_local std::vector<uint32_t> order;
    thread_local std::vector<uint32_t> next;
    starts.assign(num_shards + 1, 0);
    for (uint32_t s : shard_of) ++starts[s + 1];
    for (size_t s{0}; s < num_shards; ++s) starts[s + 1] += starts[s];
    order.resize(shard_of.size());
    next.assign(starts.begin(), starts.begin() + num_shards);
    for (uint32_t i{0}; i < shard_of.size(); ++i) order[next[shard_of[i]]++] = i;

    const size_t offset{start_hint % num_shards};
    for (size_t k{0}; k < num_shards; ++k) {
        const size_t s{(offset + k) % num_shards};
        if (starts[s] == starts[s + 1]) continue;
        Shard& shard{m_shards[s]};
        LOCK(shard.mutex);
        work(shard, std::span<const uint32_t>{order}.subspan(starts[s], starts[s + 1] - starts[s]));
    }
}

template <typename Work>
void CoinsStore::VisitShardsShared(const std::vector<uint32_t>& shard_of, ThreadPool& pool, size_t helpers, Work work) const
{
    const size_t num_shards{m_shards.size()};
    std::vector<uint32_t> starts(num_shards + 1, 0);
    for (uint32_t s : shard_of) ++starts[s + 1];
    for (size_t s{0}; s < num_shards; ++s) starts[s + 1] += starts[s];
    std::vector<uint32_t> order(shard_of.size());
    std::vector<uint32_t> next(starts.begin(), starts.begin() + num_shards);
    for (uint32_t i{0}; i < shard_of.size(); ++i) order[next[shard_of[i]]++] = i;

    std::vector<uint32_t> taken;
    taken.reserve(num_shards);
    for (size_t s{0}; s < num_shards; ++s) {
        if (starts[s] != starts[s + 1]) taken.push_back(static_cast<uint32_t>(s));
    }
    if (taken.size() <= 1 || helpers == 0) {
        for (const uint32_t s : taken) {
            Shard& shard{m_shards[s]};
            LOCK(shard.mutex);
            work(shard, std::span<const uint32_t>{order}.subspan(starts[s], starts[s + 1] - starts[s]));
        }
        return;
    }

    struct Shared {
        std::atomic<size_t> next{0};
        std::atomic<int> active{0};
        size_t count{0};
    };
    const auto shared{std::make_shared<Shared>()};
    shared->count = taken.size();

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

void CoinsStore::FetchConfirmedInto(std::span<const COutPoint> outpoints, std::vector<std::pair<COutPoint, Coin>>& found) const
{
    if (outpoints.empty()) return;
    thread_local std::vector<uint32_t> shard_of;
    thread_local std::vector<uint64_t> hashes;
    shard_of.resize(outpoints.size());
    hashes.resize(outpoints.size());
    for (size_t i{0}; i < outpoints.size(); ++i) {
        shard_of[i] = static_cast<uint32_t>(ShardIndex(outpoints[i]));
        hashes[i] = KeyHash(outpoints[i]);
    }
    const size_t start_hint{static_cast<size_t>(hashes[0])};
    VisitShards(start_hint, shard_of, [&](Shard& shard, std::span<const uint32_t> items) EXCLUSIVE_LOCKS_REQUIRED(shard.mutex) {
        if (shard.table.size() == 0) return;
        for (const uint32_t i : items) {
            const size_t pos{shard.table.Find(outpoints[i], hashes[i])};
            if (pos == NOT_FOUND) continue;
            Slot& slot{shard.table.SlotAt(pos)};
            if (slot.coin.IsSpent()) continue;
            Reference(slot);
            found.emplace_back(outpoints[i], slot.coin.ToCoin());
        }
    });
}

std::vector<std::pair<COutPoint, Coin>> CoinsStore::FetchConfirmed(std::vector<COutPoint>&& outpoints) const
{
    std::vector<std::pair<COutPoint, Coin>> found;
    found.reserve(outpoints.size());
    FetchConfirmedInto(outpoints, found);
    return found;
}

CoinsStore::BatchCheckResult CoinsStore::CommitWindowBatch(std::span<BlockChanges*> blocks, std::span<const uint256> block_hashes, ThreadPool* pool, bool publish_height)
{
    Assert(!m_run_in_flight);
    Assert(blocks.size() == block_hashes.size());
    if (blocks.empty()) return {.accepted = 0, .conflicts = 0};
    const size_t num_shards{m_shards.size()};
    size_t total_ops{0};
    for (const BlockChanges* b : blocks) total_ops += b->ops.size();
    if (total_ops == 0) {
        const size_t last{blocks.size() - 1};
        m_tier_block_hash = block_hashes[last];
        m_change_height = blocks[last]->height;
        m_tier_height = blocks[last]->height;
        if (publish_height) m_confirmed_height.store(blocks[last]->height, std::memory_order_release);
        return {.accepted = blocks.size(), .conflicts = 0};
    }

    const uint64_t batch_id{++m_commit_batch_id};
    const int written_height{m_written_height};
    const uint64_t current_epoch{m_write_out_epoch.load(std::memory_order_acquire)};
    std::atomic<size_t> first_conflict{blocks.size()};
    std::atomic<size_t> conflict_count{0};

    auto flags_of{[](const Slot& slot) noexcept -> uint8_t {
        return static_cast<uint8_t>((slot.dirty ? 1u : 0u) | (slot.fresh ? 2u : 0u) |
                                    (slot.collected ? 4u : 0u) | (slot.referenced ? 8u : 0u));
    }};

    struct FlatRef {
        uint32_t block_idx;
        uint32_t op_idx;
        CommitOp* op;
    };

    const auto commit_shard{[&](size_t s) noexcept {
        thread_local std::vector<FlatRef> flat;
        flat.clear();
        for (uint32_t bi{0}; bi < blocks.size(); ++bi) {
            BlockChanges& b{*blocks[bi]};
            if (b.shard_offsets.empty()) continue;
            const uint32_t begin{b.shard_offsets[s]};
            const uint32_t end{b.shard_offsets[s + 1]};
            for (uint32_t oi{begin}; oi < end; ++oi) {
                flat.push_back({bi, oi, &b.ops[oi]});
            }
        }
        if (flat.empty()) return;

        auto record_conflict{[&](size_t bi) {
            size_t prev{first_conflict.load(std::memory_order_relaxed)};
            while (bi < prev && !first_conflict.compare_exchange_weak(prev, bi, std::memory_order_relaxed)) {}
            conflict_count.fetch_add(1, std::memory_order_relaxed);
        }};

        Shard& shard{m_shards[s]};
        LOCK(shard.mutex);

        shard.revert_meta.clear();
        shard.revert_coins.clear();
        shard.revert_batch_id = batch_id;

        if (shard.table.NeedsGrow(flat.size())) {
            shard.table.EnsureCapacity(flat.size());
        }

        const size_t n_flat{flat.size()};
        int64_t dirty_delta{0};
        int64_t heap_delta{0};

        for (size_t k{0}; k < n_flat; ++k) {
            const FlatRef& fr{flat[k]};
            if (fr.block_idx >= first_conflict.load(std::memory_order_relaxed)) break;
            const BlockChanges& b{*blocks[fr.block_idx]};
            CommitOp& op{*fr.op};

            if (op.coin.IsSpent()) {
                const size_t pos{shard.table.Find(op.outpoint, op.key_hash)};
                if (pos != NOT_FOUND) {
                    Slot& slot{shard.table.SlotAt(pos)};
                    if (slot.coin.IsSpent()) {
                        record_conflict(fr.block_idx);
                        break;
                    }
                    const State from{StateOf(slot)};
                    const bool was_dirty{slot.dirty != 0};
                    const int32_t old_height{slot.height};
                    const uint8_t old_flags{flags_of(slot)};
                    const size_t old_heap{slot.coin.DynamicMemoryUsage()};
                    const int64_t old_dirty_bytes{DirtyBytes(slot)};
                    const uint32_t coin_idx{static_cast<uint32_t>(shard.revert_coins.size())};
                    shard.revert_coins.push_back(std::move(slot.coin));
                    shard.revert_meta.push_back({fr.block_idx, fr.op_idx, coin_idx, old_height, old_flags, /*existed=*/true});

                    if (slot.fresh) {
                        if (was_dirty) {
                            AddDirtySpanBytes(shard, old_height, -old_dirty_bytes);
                            --dirty_delta;
                        }
                        heap_delta -= static_cast<int64_t>(old_heap);
                        shard.table.EraseAt(pos);
                    } else {
                        if (was_dirty) AddDirtySpanBytes(shard, old_height, -old_dirty_bytes);
                        heap_delta -= static_cast<int64_t>(old_heap);
                        slot.coin.Clear();
                        Transit(slot, from, Event::ConfirmedSpend, op.height);
                        AddDirtySpanBytes(shard, slot.height, DirtyBytes(slot));
                        if (!was_dirty) ++dirty_delta;
                    }
                } else {
                    if (op.spent_height > written_height || (b.validate_epoch != current_epoch && !base->HaveCoin(op.outpoint))) {
                        record_conflict(fr.block_idx);
                        break;
                    }
                    auto [ins, inserted]{shard.table.TryEmplace(op.outpoint, op.key_hash)};
                    Assert(inserted);
                    shard.revert_meta.push_back({fr.block_idx, fr.op_idx, RevertMeta::NO_COIN, 0, 0, /*existed=*/false});
                    Slot& slot{shard.table.SlotAt(ins)};
                    Transit(slot, coins_entry::ABSENT, Event::ConfirmedSpend, op.height);
                    AddDirtySpanBytes(shard, slot.height, DirtyBytes(slot));
                    ++dirty_delta;
                }
            } else {
                auto [pos, inserted]{shard.table.TryEmplace(op.outpoint, op.key_hash)};
                Slot& slot{shard.table.SlotAt(pos)};
                if (inserted) {
                    if (b.must_be_new && b.validate_epoch != current_epoch && base->HaveCoin(op.outpoint)) {
                        shard.table.EraseAt(pos);
                        record_conflict(fr.block_idx);
                        break;
                    }
                    shard.revert_meta.push_back({fr.block_idx, fr.op_idx, RevertMeta::NO_COIN, 0, 0, /*existed=*/false});
                    slot.coin = std::move(op.coin);
                    Transit(slot, coins_entry::ABSENT, op.possible_overwrite ? Event::ConfirmedWrite : Event::ConfirmedNew, op.height);
                    heap_delta += static_cast<int64_t>(slot.coin.DynamicMemoryUsage());
                    AddDirtySpanBytes(shard, slot.height, DirtyBytes(slot));
                    ++dirty_delta;
                } else {
                    if ((b.must_be_new || !op.possible_overwrite) && !slot.coin.IsSpent()) {
                        record_conflict(fr.block_idx);
                        break;
                    }
                    const State from{StateOf(slot)};
                    const bool was_dirty{slot.dirty != 0};
                    const int32_t old_height{slot.height};
                    const uint8_t old_flags{flags_of(slot)};
                    const size_t old_heap{slot.coin.DynamicMemoryUsage()};
                    const int64_t old_dirty_bytes{DirtyBytes(slot)};
                    uint32_t coin_idx{RevertMeta::NO_COIN};
                    if (!slot.coin.IsSpent()) {
                        coin_idx = static_cast<uint32_t>(shard.revert_coins.size());
                        shard.revert_coins.push_back(std::move(slot.coin));
                    }
                    shard.revert_meta.push_back({fr.block_idx, fr.op_idx, coin_idx, old_height, old_flags, /*existed=*/true});

                    if (was_dirty) AddDirtySpanBytes(shard, old_height, -old_dirty_bytes);
                    heap_delta -= static_cast<int64_t>(old_heap);
                    slot.coin = std::move(op.coin);
                    Transit(slot, from, Event::ConfirmedWrite, op.height);
                    heap_delta += static_cast<int64_t>(slot.coin.DynamicMemoryUsage());
                    AddDirtySpanBytes(shard, slot.height, DirtyBytes(slot));
                    if (!was_dirty) ++dirty_delta;
                }
            }
        }
        if (heap_delta > 0) {
            AddUsage(shard, static_cast<size_t>(heap_delta));
        } else if (heap_delta < 0) {
            SubUsage(shard, static_cast<size_t>(-heap_delta));
        }
        SyncDirtyCount(shard, dirty_delta);
        SyncTableStats(shard);
    }};

    const size_t helpers{pool && total_ops >= COMMIT_OPS_PER_HELPER
                             ? std::min(pool->WorkersCount(), total_ops / COMMIT_OPS_PER_HELPER)
                             : 0};
    [&]() noexcept {
        if (helpers > 0) {
            static constexpr size_t CHUNK{4};
            struct Shared {
                std::atomic<size_t> next{0};
                std::atomic<int> active{0};
                size_t count{0};
            };
            const auto shared{std::make_shared<Shared>()};
            shared->count = num_shards;
            for (size_t h{0}; h < helpers; ++h) {
                (void)pool->Submit([shared, &commit_shard]() noexcept {
                    shared->active.fetch_add(1);
                    for (size_t base{shared->next.fetch_add(CHUNK, std::memory_order_relaxed)}; base < shared->count; base = shared->next.fetch_add(CHUNK, std::memory_order_relaxed)) {
                        const size_t end{std::min(base + CHUNK, shared->count)};
                        WAITED_FOR_UNDER_LOCK("a window batch commit helper");
                        for (size_t s{base}; s < end; ++s) {
                            commit_shard(s);
                        }
                    }
                    if (shared->active.fetch_sub(1) == 1) shared->active.notify_all();
                }, /*front=*/true);
            }
            for (size_t base{shared->next.fetch_add(CHUNK, std::memory_order_relaxed)}; base < shared->count; base = shared->next.fetch_add(CHUNK, std::memory_order_relaxed)) {
                const size_t end{std::min(base + CHUNK, shared->count)};
                for (size_t s{base}; s < end; ++s) {
                    commit_shard(s);
                }
            }
            for (int active{shared->active.load()}; active != 0; active = shared->active.load()) shared->active.wait(active);
        } else {
            for (size_t s{0}; s < num_shards; ++s) commit_shard(s);
        }
    }();

    const size_t accepted{first_conflict.load(std::memory_order_relaxed)};
    if (accepted < blocks.size()) {
        // Rare conflict path: roll back every shard's mutations for block_idx >= accepted in exact reverse order.
        for (Shard& shard : m_shards) {
            LOCK(shard.mutex);
            if (shard.revert_batch_id != batch_id) continue;
            while (!shard.revert_meta.empty() && shard.revert_meta.back().block_idx >= accepted) {
                const RevertMeta r{shard.revert_meta.back()};
                shard.revert_meta.pop_back();
                CommitOp& op{blocks[r.block_idx]->ops[r.op_idx]};
                if (!r.existed) {
                    const size_t pos{shard.table.Find(op.outpoint, op.key_hash)};
                    Assert(pos != NOT_FOUND);
                    EraseSlot(shard, pos);
                } else {
                    auto [pos, inserted]{shard.table.TryEmplace(op.outpoint, op.key_hash)};
                    Slot& slot{shard.table.SlotAt(pos)};
                    if (!inserted) {
                        if (slot.dirty) {
                            AddDirtySpanBytes(shard, slot.height, -DirtyBytes(slot));
                            SyncDirtyCount(shard, -1);
                        }
                        SubUsage(shard, slot.coin.DynamicMemoryUsage());
                    }
                    if (r.coin_idx != RevertMeta::NO_COIN) {
                        slot.coin = std::move(shard.revert_coins[r.coin_idx]);
                    } else {
                        slot.coin.Clear();
                    }
                    slot.height = r.old_height;
                    slot.dirty = (r.old_flags & 1u) ? 1 : 0;
                    slot.fresh = (r.old_flags & 2u) ? 1 : 0;
                    slot.collected = (r.old_flags & 4u) ? 1 : 0;
                    slot.referenced = (r.old_flags & 8u) ? 1 : 0;
                    AddUsage(shard, slot.coin.DynamicMemoryUsage());
                    if (slot.dirty) {
                        AddDirtySpanBytes(shard, slot.height, DirtyBytes(slot));
                        SyncDirtyCount(shard, 1);
                    }
                }
            }
            shard.revert_meta.clear();
            shard.revert_coins.clear();
            SyncTableStats(shard);
        }
    }

    if (accepted > 0) {
        const size_t last{accepted - 1};
        m_tier_block_hash = block_hashes[last];
        m_change_height = blocks[last]->height;
        m_tier_height = blocks[last]->height;
        if (publish_height) m_confirmed_height.store(blocks[last]->height, std::memory_order_release);
    }
    return {.accepted = accepted, .conflicts = conflict_count.load(std::memory_order_relaxed)};
}

void CoinsStore::CommitBatch(std::span<CommitOp> ops, const uint256& best_hash, int best_height, ThreadPool* pool, bool publish_height)
{
    Assert(!m_run_in_flight);
    std::vector<uint32_t> active_idx;
    std::vector<uint32_t> shard_of;
    active_idx.reserve(ops.size());
    shard_of.reserve(ops.size());
    for (uint32_t i{0}; i < ops.size(); ++i) {
        if (ops[i].cancelled) continue;
        active_idx.push_back(i);
        shard_of.push_back(ops[i].shard);
    }

    if (!active_idx.empty()) {
        const auto apply_shard{[&](Shard& shard, std::span<const uint32_t> items) EXCLUSIVE_LOCKS_REQUIRED(shard.mutex) {
            shard.table.EnsureCapacity(items.size());
            for (const uint32_t item : items) {
                CommitOp& op{ops[active_idx[item]]};
                if (op.coin.IsSpent()) {
                    const size_t pos{shard.table.Find(op.outpoint, op.key_hash)};
                    if (pos != NOT_FOUND) {
                        Slot& slot{shard.table.SlotAt(pos)};
                        const State from{StateOf(slot)};
                        if (slot.fresh) {
                            EraseSlot(shard, pos);
                        } else {
                            const bool was_dirty{slot.dirty != 0};
                            if (was_dirty) AddDirtySpanBytes(shard, slot.height, -DirtyBytes(slot));
                            SubUsage(shard, slot.coin.DynamicMemoryUsage());
                            slot.coin.Clear();
                            Transit(slot, from, Event::ConfirmedSpend, op.height);
                            AddDirtySpanBytes(shard, slot.height, DirtyBytes(slot));
                            if (!was_dirty) SyncDirtyCount(shard, 1);
                        }
                    } else {
                        auto [ins, inserted]{shard.table.TryEmplace(op.outpoint, op.key_hash)};
                        Assert(inserted);
                        Slot& slot{shard.table.SlotAt(ins)};
                        Transit(slot, coins_entry::ABSENT, Event::ConfirmedSpend, op.height);
                        AddDirtySpanBytes(shard, slot.height, DirtyBytes(slot));
                        SyncDirtyCount(shard, 1);
                    }
                } else {
                    auto [pos, inserted]{shard.table.TryEmplace(op.outpoint, op.key_hash)};
                    Slot& slot{shard.table.SlotAt(pos)};
                    const State from{inserted ? coins_entry::ABSENT : StateOf(slot)};
                    if (inserted) {
                        slot.coin = std::move(op.coin);
                        Transit(slot, from, op.possible_overwrite ? Event::ConfirmedWrite : Event::ConfirmedNew, op.height);
                        AddUsage(shard, slot.coin.DynamicMemoryUsage());
                        AddDirtySpanBytes(shard, slot.height, DirtyBytes(slot));
                        SyncDirtyCount(shard, 1);
                    } else {
                        Assert(op.possible_overwrite || slot.coin.IsSpent());
                        const bool was_dirty{slot.dirty != 0};
                        if (was_dirty) AddDirtySpanBytes(shard, slot.height, -DirtyBytes(slot));
                        SubUsage(shard, slot.coin.DynamicMemoryUsage());
                        slot.coin = std::move(op.coin);
                        Transit(slot, from, Event::ConfirmedWrite, op.height);
                        AddUsage(shard, slot.coin.DynamicMemoryUsage());
                        AddDirtySpanBytes(shard, slot.height, DirtyBytes(slot));
                        if (!was_dirty) SyncDirtyCount(shard, 1);
                    }
                }
            }
            SyncTableStats(shard);
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
// Sizing, linear clock eviction, and write-out runs

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

int CoinsStore::BoundForBytes(size_t bytes, int tip_height) const
{
    std::vector<int64_t> merged;
    for (const Shard& shard : m_shards) {
        LOCK(shard.mutex);
        if (merged.size() < shard.dirty_span_bytes.size()) merged.resize(shard.dirty_span_bytes.size(), 0);
        for (size_t span{0}; span < shard.dirty_span_bytes.size(); ++span) merged[span] += shard.dirty_span_bytes[span];
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
            if (span >= shard.dirty_span_bytes.size() || shard.dirty_span_bytes[span] <= 0) continue;
            const size_t cap{shard.table.cap()};
            for (size_t i{0}; i < cap; ++i) {
                if (!shard.table.IsOccupiedAt(i)) continue;
                const Slot& slot{shard.table.SlotAt(i)};
                if (slot.dirty && SpanOf(slot.height) == span) {
                    by_height[slot.height] += DirtyBytes(slot);
                }
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
    size_t usage{DynamicMemoryUsage()};
    if (usage <= target_bytes) return 0;
    size_t freed{0};
    size_t est_freed_bytes{0};
    // Bounded 2-pass linear clock sweep per shard; ShrinkToFit reclaims real memory via munmap.
    for (int pass{0}; pass < 2 && usage > target_bytes + est_freed_bytes; ++pass) {
        bool progress{false};
        for (Shard& shard : m_shards) {
            if (target_bytes > 0 && usage <= target_bytes + est_freed_bytes) break;
            LOCK(shard.mutex);
            const size_t cap{shard.table.cap()};
            if (cap == 0 || shard.table.size() <= shard.dirty_count) continue;
            size_t hand{shard.clock_hand % cap};
            size_t evicted_here{0};
            for (size_t step{0}; step < cap; ++step) {
                if (target_bytes > 0 && usage <= target_bytes + est_freed_bytes) break;
                const size_t idx{hand};
                hand = (hand + 1) & (cap - 1);
                if (!shard.table.IsOccupiedAt(idx)) continue;
                Slot& slot{shard.table.SlotAt(idx)};
                if (slot.dirty) continue;
                if (slot.referenced) {
                    slot.referenced = 0;
                    progress = true;
                    continue;
                }
                const size_t entry_bytes{static_cast<size_t>(DirtyBytes(slot))};
                EraseSlot(shard, idx);
                ++freed;
                ++evicted_here;
                est_freed_bytes += entry_bytes;
                progress = true;
            }
            shard.clock_hand = hand;
            if (evicted_here > 0 && shard.table.ShouldShrink()) {
                shard.table.ShrinkToFit();
                SyncTableStats(shard);
                usage = DynamicMemoryUsage();
                est_freed_bytes = 0;
                shard.clock_hand = 0;
            }
        }
        if (!progress) break;
    }
    return freed;
}

std::unique_ptr<CoinsStore::WriteOutRun> CoinsStore::BeginWriteOut(int bound_height, int head_height, const uint256& new_head, bool final)
{
    Assert(!m_run_in_flight);
    final = final || m_fell_below_written;
    if (!final) {
        bool any{!m_restored.empty()};
        if (!any) {
            for (const Shard& shard : m_shards) {
                LOCK(shard.mutex);
                if (shard.dirty_count == 0) continue;
                for (size_t span{0}; span < shard.dirty_span_bytes.size(); ++span) {
                    if (shard.dirty_span_bytes[span] <= 0) continue;
                    const int span_start{static_cast<int>(span << DIRTY_SPAN_BITS)};
                    const int span_end{static_cast<int>(((span + 1) << DIRTY_SPAN_BITS) - 1)};
                    if (span_start > bound_height) break;
                    if (span_end <= bound_height) {
                        any = true;
                        break;
                    }
                    // bound_height falls inside this first non-empty span: check exact heights.
                    const size_t cap{shard.table.cap()};
                    for (size_t i{0}; i < cap; ++i) {
                        if (!shard.table.IsOccupiedAt(i)) continue;
                        const Slot& slot{shard.table.SlotAt(i)};
                        if (slot.dirty && slot.height <= bound_height) {
                            any = true;
                            break;
                        }
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
    for (size_t i{from}; i < writes.size(); ++i) {
        sorter.order.emplace_back(ReadBE64(writes[i].outpoint.hash.ToUint256().data()), static_cast<uint32_t>(i));
    }
    std::sort(sorter.order.begin(), sorter.order.end(), [&](const auto& a, const auto& b) {
        if (a.first != b.first) return a.first > b.first;
        return writes[b.second].outpoint < writes[a.second].outpoint;
    });
    sorter.sorted.clear();
    sorter.sorted.reserve(count);
    for (const auto& [prefix, index] : sorter.order) sorter.sorted.push_back(std::move(writes[index]));
    for (size_t i{0}; i < count; ++i) writes[from + i] = std::move(sorter.sorted[i]);
}

void CoinsStore::KeepWritten(std::vector<CoinWrite> restored)
{
    for (CoinWrite& write : restored) {
        Shard& shard{ShardFor(write.outpoint)};
        LOCK(shard.mutex);
        if (const size_t pos{shard.table.Find(write.outpoint)}; pos != NOT_FOUND) {
            Slot& slot{shard.table.SlotAt(pos)};
            if (slot.fresh) Transit(slot, Event::KeptWritten);
        }
        m_restored.push_back(std::move(write));
    }
}

void CoinsStore::CollectChunk(Shard& shard, size_t& cursor, WriteOutRun& run, uint64_t limit, uint64_t& size, std::vector<CoinWrite>& writes, std::vector<SettledRef>& settled) const
{
    AssertLockHeld(shard.mutex);
    const size_t cap{shard.table.cap()};
    const uint64_t gen{shard.table.Gen()};
    while (cursor < cap) {
        if (!shard.table.IsOccupiedAt(cursor)) {
            ++cursor;
            continue;
        }
        Slot& slot{shard.table.SlotAt(cursor)};
        if (!slot.dirty || slot.collected) {
            ++cursor;
            continue;
        }
        if (!run.final && slot.height > run.bound_height) {
            run.oldest_left = std::min<int>(run.oldest_left, slot.height);
            ++cursor;
            continue;
        }
        if (size >= limit) return;

        // CS-5: confirmed coin created above the bound that overwrote an older owed erase: write the
        // erase now and move the entry to its coin's height so a partial run never writes a future coin.
        if (!run.final && !slot.coin.IsSpent() && !slot.fresh && static_cast<int>(slot.coin.nHeight) > run.bound_height) {
            const int created{static_cast<int>(slot.coin.nHeight)};
            const int64_t bytes{DirtyBytes(slot)};
            writes.push_back({slot.key, std::nullopt});
            size += BatchBytes(writes.back());
            ++run.count;
            AddDirtySpanBytes(shard, slot.height, -bytes);
            Transit(slot, Event::RunWritesOwed, created);
            AddDirtySpanBytes(shard, slot.height, bytes);
            run.oldest_left = std::min<int>(run.oldest_left, created);
            ++cursor;
            continue;
        }
        writes.push_back({slot.key, slot.coin.IsSpent() ? std::nullopt : std::make_optional(slot.coin.ToCoin())});
        settled.push_back({cursor, KeyHash(slot.key), gen, slot.key});
        size += BatchBytes(writes.back());
        ++run.count;
        run.bytes += static_cast<size_t>(DirtyBytes(slot));
        Transit(slot, Event::RunCollects);
        ++cursor;
    }
}

void CoinsStore::SettleSlot(Shard& shard, size_t pos) const
{
    AssertLockHeld(shard.mutex);
    Slot& slot{shard.table.SlotAt(pos)};
    if (!slot.collected) return;
    if (slot.coin.IsSpent()) {
        EraseSlot(shard, pos);
        return;
    }
    AddDirtySpanBytes(shard, slot.height, -DirtyBytes(slot));
    Transit(slot, Event::RunLands);
    SyncDirtyCount(shard, -1);
}

void CoinsStore::StreamWriteOut(WriteOutRun& run, const std::function<uint256(int)>& block_at, uint64_t batch_bytes)
{
    const uint64_t limit{std::max<uint64_t>(batch_bytes, 1)};
    uint64_t size{0};
    std::vector<CoinWrite> writes;
    std::vector<SettledRef> settled;
    std::vector<std::pair<Shard*, size_t>> spans;
    WriteSorter sorter;
    const auto write_chunk{[&] {
        if (writes.empty()) return;
        SortWrites(writes, 0, sorter);
        base->WriteRun(writes, run.new_head, run.old_head, /*final=*/false);
        ++run.chunks;
        for (size_t i{0}; i < spans.size(); ++i) {
            Shard& shard{*spans[i].first};
            const size_t end{i + 1 < spans.size() ? spans[i + 1].second : settled.size()};
            LOCK(shard.mutex);
            const uint64_t cur_gen{shard.table.Gen()};
            for (size_t n{spans[i].second}; n < end; ++n) {
                const SettledRef& ref{settled[n]};
                size_t pos{NOT_FOUND};
                if (ref.gen == cur_gen && ref.pos < shard.table.cap() &&
                    shard.table.IsOccupiedAt(ref.pos) && shard.table.SlotAt(ref.pos).key == ref.key) {
                    pos = ref.pos;
                } else {
                    pos = shard.table.Find(ref.key, ref.hash);
                }
                if (pos != NOT_FOUND) SettleSlot(shard, pos);
            }
            ++shard.settles;
        }
        writes.clear();
        settled.clear();
        spans.clear();
        size = 0;
    }};
    for (size_t index{m_shards.size()}; index-- > 0;) {
        Shard& shard{m_shards[index]};
        size_t cursor{0};
        while (true) {
            const size_t before{settled.size()};
            bool done{false};
            {
                LOCK(shard.mutex);
                CollectChunk(shard, cursor, run, limit, size, writes, settled);
                done = cursor >= shard.table.cap();
            }
            if (settled.size() > before) spans.emplace_back(&shard, before);
            if (size < limit && done) break;
            if (size >= limit) write_chunk();
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
    shard.dirty_span_bytes.clear();
    MyTotals().dirty.fetch_sub(static_cast<int64_t>(shard.dirty_count), std::memory_order_relaxed);
    shard.dirty_count = 0;
    uint64_t added{0};
    const size_t from{worker.writes.size()};
    const size_t cap{shard.table.cap()};
    for (size_t i{0}; i < cap; ++i) {
        if (!shard.table.IsOccupiedAt(i)) continue;
        Slot& slot{shard.table.SlotAt(i)};
        Transit(slot, Event::Flush);
        if (!slot.dirty) continue;
        CoinWrite w{slot.key, slot.coin.IsSpent() ? std::nullopt : std::make_optional(slot.coin.ToCoin())};
        const uint64_t bytes{BatchBytes(w)};
        added += bytes;
        worker.size += bytes;
        worker.writes.push_back(std::move(w));
    }
    SubUsage(shard, shard.heap_bytes);
    shard.table.Clear();
    shard.revert_meta.clear();
    shard.revert_coins.clear();
    shard.revert_batch_id = 0;
    shard.clock_hand = 0;
    SyncTableStats(shard);
    ++shard.settles;
    SortWrites(worker.writes, from, worker.sorter);
    return added;
}

void CoinsStore::RunWipeWorker(Wipe& wipe, WipeWorker& worker)
{
    try {
        for (size_t index{worker.end}; index-- > worker.begin;) {
            if (wipe.failed.load(std::memory_order_relaxed)) break;
            Shard& shard{m_shards[index]};
            shard.mutex.lock();
            worker.held.push_back(&shard);
            if (WipeShard(shard, worker) == 0) {
                worker.held.pop_back();
                shard.mutex.unlock();
                continue;
            }
            if (worker.size >= wipe.limit) WriteWipeBatch(wipe, worker);
        }
        if (!wipe.failed.load(std::memory_order_relaxed)) WriteWipeBatch(wipe, worker);
    } catch (...) {
        wipe.failed.store(true, std::memory_order_relaxed);
        worker.error = std::current_exception();
    }
    for (Shard* shard : worker.held) shard->mutex.unlock();
    worker.held.clear();
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
    const uint256 best{GetBestBlock()};
    const int head_height{m_tier_height >= 0 ? m_tier_height : m_change_height};
    auto run{BeginWriteOut(head_height, head_height, best, /*final=*/true)};
    if (!run) return;
    try {
        StreamWriteOut(*run, [&](int) { return best; }, m_batch_bytes);
    } catch (...) {
        AbandonWriteOut();
        throw;
    }
    CommitWriteOut(*run);
}

void CoinsStore::SanityCheck() const
{
    size_t total_bytes{0};
    size_t total_entries{0};
    size_t total_dirty{0};
    for (size_t s{0}; s < m_shards.size(); ++s) {
        const Shard& shard{m_shards[s]};
        LOCK(shard.mutex);
        assert(shard.entries == shard.table.size());
        assert(shard.table_bytes == shard.table.MemoryBytes());
        size_t occupied{0};
        size_t dirty{0};
        size_t heap{0};
        std::map<size_t, int64_t> span_bytes;
        const size_t cap{shard.table.cap()};
        for (size_t i{0}; i < cap; ++i) {
            if (!shard.table.IsOccupiedAt(i)) continue;
            const Slot& slot{shard.table.SlotAt(i)};
            assert(ShardIndex(slot.key) == s);
            assert(coins_entry::IsStored(StateOf(slot)));
            ++occupied;
            heap += slot.coin.DynamicMemoryUsage();
            if (slot.dirty) {
                ++dirty;
                span_bytes[SpanOf(slot.height)] += DirtyBytes(slot);
            }
        }
        assert(occupied == shard.entries);
        assert(dirty == shard.dirty_count);
        assert(heap == shard.heap_bytes);
        for (size_t span{0}; span < shard.dirty_span_bytes.size(); ++span) {
            const int64_t expected{span_bytes.contains(span) ? span_bytes[span] : 0};
            assert(shard.dirty_span_bytes[span] == expected);
            span_bytes.erase(span);
        }
        assert(span_bytes.empty());
        total_entries += occupied;
        total_dirty += dirty;
        total_bytes += shard.table_bytes + shard.heap_bytes;
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
    if (cacheCoins.bucket_count() < outpoints.size() * 2) cacheCoins.reserve(outpoints.size() * 2);

    thread_local std::vector<std::pair<COutPoint, Coin>> found;
    found.clear();
    if (m_overlay) {
        thread_local std::vector<COutPoint> misses;
        misses.clear();
        misses.reserve(outpoints.size());
        m_overlay->Fetch(m_height, outpoints, found, misses);
        for (auto& [outpoint, coin] : found) {
            const size_t usage{coin.DynamicMemoryUsage()};
            if (cacheCoins.try_emplace(outpoint, std::move(coin)).second) cachedCoinsUsage += usage;
        }
        outpoints.swap(misses);
    }
    if (!outpoints.empty()) {
        found.clear();
        m_store.FetchConfirmedInto(outpoints, found);
        for (auto& [outpoint, coin] : found) {
            const size_t usage{coin.DynamicMemoryUsage()};
            if (cacheCoins.try_emplace(outpoint, std::move(coin)).second) cachedCoinsUsage += usage;
        }
    }
}

bool BlockApplyView::SpendCoin(const COutPoint& outpoint, Coin* moveto)
{
    if (auto it{cacheCoins.find(outpoint)}; it != cacheCoins.end()) {
        const uint32_t created_height{it->second.coin.nHeight};
        const bool was_spent{it->second.coin.IsSpent()};
        Assume(TrySub(m_dirty_count, it->second.IsDirty()));
        Assume(TrySub(cachedCoinsUsage, it->second.coin.DynamicMemoryUsage()));
        if (moveto) *moveto = std::move(it->second.coin);
        if (it->second.IsFresh()) {
            cacheCoins.erase(it);
        } else {
            CCoinsCacheEntry::SetDirty(*it, m_sentinel);
            ++m_dirty_count;
            it->second.coin.Clear();
            if (!was_spent) it->second.coin.nHeight = created_height;
        }
        return true;
    }
    Coin tmp;
    Coin* const target{moveto ? moveto : &tmp};
    const bool spent{CCoinsViewCache::SpendCoin(outpoint, target)};
    if (spent && !target->IsSpent()) {
        if (auto it{cacheCoins.find(outpoint)}; it != cacheCoins.end()) {
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
        struct DirtyRef {
            CCoinsMap::iterator it;
            uint64_t hash;
            uint32_t shard;
        };
        thread_local std::vector<DirtyRef> refs;
        thread_local std::vector<uint32_t> next;
        refs.clear();
        refs.reserve(m_dirty_count);
        out.shard_offsets.assign(num_shards + 1, 0);
        for (auto it{cacheCoins.begin()}; it != cacheCoins.end(); ++it) {
            if (!it->second.IsDirty()) continue;
            const uint32_t s{m_store.ShardOf(it->first)};
            ++out.shard_offsets[s + 1];
            refs.push_back({it, CoinsStore::KeyHashOf(it->first), s});
        }
        for (size_t s{0}; s < num_shards; ++s) out.shard_offsets[s + 1] += out.shard_offsets[s];
        next.assign(out.shard_offsets.begin(), out.shard_offsets.begin() + num_shards);
        out.ops.resize(refs.size());
        for (const DirtyRef& r : refs) {
            CCoinsCacheEntry& entry{r.it->second};
            const bool is_spent{entry.coin.IsSpent()};
            const int32_t spent_h{is_spent ? static_cast<int32_t>(entry.coin.nHeight) : -1};
            if (is_spent) entry.coin.nHeight = 0;
            const bool may_overwrite{!entry.IsFresh() && !is_spent && entry.coin.IsCoinBase()};
            out.ops[next[r.shard]++] = CoinsStore::CommitOp{
                .outpoint = r.it->first,
                .coin = std::move(entry.coin),
                .key_hash = r.hash,
                .shard = r.shard,
                .height = m_height,
                .spent_height = spent_h,
                .possible_overwrite = may_overwrite,
                .cancelled = false,
            };
        }
    }
    const size_t entries{cacheCoins.size()};
    cacheCoins.clear();
    ReallocateCache();
    cacheCoins.reserve(entries);
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
    m_store.CommitBatch(block_changes.ops, block_hash, m_height, /*pool=*/nullptr, /*publish_height=*/true);
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
