// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <ibd_window.h>

#include <chain.h>
#include <core_memusage.h>
#include <crypto/common.h>
#include <memusage.h>
#include <random.h>
#include <serialize.h>
#include <util/check.h>
#include <util/log.h>
#include <util/threadpool.h>
#include <validation.h>

#include <algorithm>
#include <cassert>
#include <cstring>
#include <limits>
#include <mutex>
#include <thread>
#include <unordered_set>

size_t EstimateBlockMemoryUsage(const CBlock& block) noexcept
{
    size_t bytes = sizeof(CBlock) + memusage::DynamicUsage(block.vtx);
    for (const auto& tx : block.vtx) {
        if (!tx) continue;
        bytes += RecursiveDynamicUsage(*tx) + sizeof(CTransaction);
    }
    return bytes;
}

void BlockHandoffCache::Put(const uint256& hash, std::shared_ptr<const CBlock> block, size_t max_bytes)
{
    if (!block || max_bytes == 0) return;
    const size_t bytes = EstimateBlockMemoryUsage(*block);
    if (bytes > max_bytes) return;

    LOCK(m_mutex);
    if (m_map.contains(hash)) return;

    while (!m_fifo.empty() && m_total_bytes + bytes > max_bytes) {
        const Entry& oldest = m_fifo.front();
        m_total_bytes -= oldest.bytes;
        m_budget.handoff_bytes.fetch_sub(oldest.bytes, std::memory_order_relaxed);
        m_map.erase(oldest.hash);
        m_fifo.pop_front();
    }

    m_fifo.push_back(Entry{hash, std::move(block), bytes});
    auto it = std::prev(m_fifo.end());
    m_map.emplace(hash, it);
    m_total_bytes += bytes;
    m_budget.handoff_bytes.fetch_add(bytes, std::memory_order_relaxed);
    m_count.store(m_fifo.size(), std::memory_order_release);
}

std::shared_ptr<const CBlock> BlockHandoffCache::Take(const uint256& hash)
{
    if (m_count.load(std::memory_order_acquire) == 0) return nullptr;

    LOCK(m_mutex);
    auto it = m_map.find(hash);
    if (it == m_map.end()) return nullptr;

    auto list_it = it->second;
    std::shared_ptr<const CBlock> block = std::move(list_it->block);
    const size_t bytes = list_it->bytes;
    m_map.erase(it);
    m_fifo.erase(list_it);
    m_total_bytes -= bytes;
    m_budget.handoff_bytes.fetch_sub(bytes, std::memory_order_relaxed);
    m_count.store(m_fifo.size(), std::memory_order_release);
    return block;
}

void BlockHandoffCache::Clear()
{
    LOCK(m_mutex);
    m_map.clear();
    m_fifo.clear();
    if (m_total_bytes > 0) {
        m_budget.handoff_bytes.fetch_sub(m_total_bytes, std::memory_order_relaxed);
        m_total_bytes = 0;
    }
    m_count.store(0, std::memory_order_release);
}

WindowOutputTable::WindowOutputTable(IBDMemoryBudget& budget, size_t min_ring_size)
    : m_budget{budget},
      m_txid_hasher{FastRandomContext().rand64(), FastRandomContext().rand64()},
      m_ring_size{std::bit_ceil(std::max<size_t>(min_ring_size, MIN_RING_SIZE))},
      m_ring_mask{m_ring_size - 1},
      m_ring{std::make_unique<HeldBlock[]>(m_ring_size)}
{
}

void WindowOutputTable::UpdateShardBudget(Shard& shard) noexcept
{
    const size_t current = shard.slots.capacity() * sizeof(EntrySlot);
    if (current > shard.reported_bytes) {
        m_budget.window_outputs_bytes.fetch_add(current - shard.reported_bytes, std::memory_order_relaxed);
    } else if (current < shard.reported_bytes) {
        m_budget.window_outputs_bytes.fetch_sub(shard.reported_bytes - current, std::memory_order_relaxed);
    }
    shard.reported_bytes = current;
}

void WindowOutputTable::InsertBlockOutputs(int height, std::shared_ptr<const CBlock> block)
{
    HeldBlock& held = m_ring[static_cast<size_t>(height) & m_ring_mask];
    while (held.ptr.load(std::memory_order_acquire) != nullptr) {
        if (!EraseOne()) std::this_thread::yield();
    }

    const CBlock& blk = *Assert(block);
    const size_t num_tx = blk.vtx.size();

    static thread_local std::vector<uint64_t> tags;
    static thread_local std::vector<uint32_t> order;
    tags.resize(num_tx);
    order.resize(num_tx);

    std::array<uint32_t, NUM_SHARDS> counts{};
    for (size_t i = 0; i < num_tx; ++i) {
        const uint64_t t = TagOf(blk.vtx[i]->GetHash());
        tags[i] = t;
        ++counts[ShardOf(t)];
    }

    std::array<uint32_t, NUM_SHARDS + 1> offsets{};
    for (size_t s = 0; s < NUM_SHARDS; ++s) {
        offsets[s + 1] = offsets[s] + counts[s];
    }
    std::array<uint32_t, NUM_SHARDS> write_pos{};
    for (size_t s = 0; s < NUM_SHARDS; ++s) write_pos[s] = offsets[s];
    for (uint32_t i = 0; i < static_cast<uint32_t>(num_tx); ++i) {
        order[write_pos[ShardOf(tags[i])]++] = i;
    }

    held.height.store(height, std::memory_order_relaxed);
    held.owner = std::move(block);
    held.ptr.store(&blk, std::memory_order_release);

    const size_t start = m_shard_cursor.fetch_add(1, std::memory_order_relaxed) & (NUM_SHARDS - 1);
    for (size_t k = 0; k < NUM_SHARDS; ++k) {
        const size_t s = (start + k) & (NUM_SHARDS - 1);
        const uint32_t begin = offsets[s];
        const uint32_t end = offsets[s + 1];
        const size_t add = end - begin;
        if (add == 0) continue;

        Shard& shard = m_shards[s];
        std::unique_lock lock{shard.mutex};
        size_t cap = shard.slots.size();
        while ((shard.count + add) * 2 > cap) {
            cap = std::max<size_t>(32, cap * 2);
        }
        if (cap != shard.slots.size()) {
            std::vector<EntrySlot> new_slots(cap);
            const size_t new_mask = cap - 1;
            for (const EntrySlot& existing : shard.slots) {
                if (existing.tag == 0) continue;
                size_t at = static_cast<size_t>(existing.tag >> 1) & new_mask;
                while (new_slots[at].tag != 0) at = (at + 1) & new_mask;
                new_slots[at] = existing;
            }
            shard.slots.swap(new_slots);
            shard.mask = new_mask;
            UpdateShardBudget(shard);
        }

        const size_t mask = shard.mask;
        for (uint32_t p = begin; p < end; ++p) {
            const uint32_t tx_idx = order[p];
            const uint64_t tag = tags[tx_idx];
            size_t at = static_cast<size_t>(tag >> 1) & mask;
            while (shard.slots[at].tag != 0) at = (at + 1) & mask;
            shard.slots[at] = EntrySlot{tag, tx_idx, height};
        }
        shard.count += add;
    }
}

void WindowOutputTable::LookupBatch(int consumer_height,
                                    std::span<const COutPoint> outpoints,
                                    std::span<std::optional<Coin>> results,
                                    std::span<const uint64_t> precomputed_tags) const
{
    Assert(outpoints.size() == results.size());
    Assert(precomputed_tags.empty() || precomputed_tags.size() == outpoints.size());
    const size_t n = outpoints.size();
    if (n == 0) return;

    const int published = m_published.load(std::memory_order_acquire);
    if (published + 1 >= consumer_height) return;

    auto probe_one = [&](const Shard& shard, uint64_t tag, const COutPoint& op) noexcept -> std::optional<Coin> {
        if (shard.count == 0) return std::nullopt;
        const size_t mask = shard.mask;
        const CTransaction* best_tx = nullptr;
        int best_height = published;
        for (size_t at = static_cast<size_t>(tag >> 1) & mask; shard.slots[at].tag != 0; at = (at + 1) & mask) {
            const EntrySlot& slot = shard.slots[at];
            if (slot.tag != tag) continue;
            if (slot.height <= best_height || slot.height >= consumer_height) continue;
            const CBlock* blk = m_ring[static_cast<size_t>(slot.height) & m_ring_mask].ptr.load(std::memory_order_relaxed);
            const CTransaction& tx = *blk->vtx[slot.tx_idx];
            if (tx.GetHash() != op.hash) continue;
            best_tx = &tx;
            best_height = slot.height;
        }
        if (!best_tx) return std::nullopt;
        if (op.n < best_tx->vout.size() && !best_tx->vout[op.n].scriptPubKey.IsUnspendable()) {
            return Coin{best_tx->vout[op.n], best_height, best_tx->IsCoinBase()};
        }
        return std::nullopt;
    };

    if (n == 1) {
        const uint64_t tag = precomputed_tags.empty() ? TagOf(outpoints[0].hash) : precomputed_tags[0];
        const Shard& shard = m_shards[ShardOf(tag)];
        std::shared_lock lock{shard.mutex};
        if (auto coin = probe_one(shard, tag, outpoints[0])) {
            results[0] = std::move(*coin);
        }
        return;
    }

    static thread_local std::vector<uint64_t> local_tags;
    static thread_local std::vector<uint32_t> order;
    std::span<const uint64_t> tags;
    if (!precomputed_tags.empty()) {
        tags = precomputed_tags;
    } else {
        local_tags.resize(n);
        for (size_t i = 0; i < n; ++i) {
            local_tags[i] = TagOf(outpoints[i].hash);
        }
        tags = local_tags;
    }
    order.resize(n);

    std::array<uint32_t, NUM_SHARDS> counts{};
    for (size_t i = 0; i < n; ++i) {
        ++counts[ShardOf(tags[i])];
    }
    std::array<uint32_t, NUM_SHARDS + 1> offsets{};
    for (size_t s = 0; s < NUM_SHARDS; ++s) {
        offsets[s + 1] = offsets[s] + counts[s];
    }
    std::array<uint32_t, NUM_SHARDS> write_pos{};
    for (size_t s = 0; s < NUM_SHARDS; ++s) write_pos[s] = offsets[s];
    for (uint32_t i = 0; i < static_cast<uint32_t>(n); ++i) {
        order[write_pos[ShardOf(tags[i])]++] = i;
    }

    const size_t start = m_shard_cursor.fetch_add(1, std::memory_order_relaxed) & (NUM_SHARDS - 1);
    for (size_t k = 0; k < NUM_SHARDS; ++k) {
        const size_t s = (start + k) & (NUM_SHARDS - 1);
        const uint32_t begin = offsets[s];
        const uint32_t end = offsets[s + 1];
        if (begin == end) continue;

        const Shard& shard = m_shards[s];
        std::shared_lock lock{shard.mutex};
        if (shard.count == 0) continue;

        for (uint32_t p = begin; p < end; ++p) {
            const uint32_t idx = order[p];
            if (auto coin = probe_one(shard, tags[idx], outpoints[idx])) {
                results[idx] = std::move(*coin);
            }
        }
    }
}

std::optional<COutPoint> WindowOutputTable::FindBIP30ConflictBelowHeight(int consumer_height, const CBlock& block) const
{
    const size_t num_tx = block.vtx.size();
    if (num_tx == 0) return std::nullopt;
    const int published = m_published.load(std::memory_order_acquire);
    if (published + 1 >= consumer_height) return std::nullopt;

    auto check_tx_in_shard = [&](const Shard& shard, uint64_t tag, const CTransaction& query_tx) noexcept -> std::optional<COutPoint> {
        if (shard.count == 0) return std::nullopt;
        const Txid& txid = query_tx.GetHash();
        const size_t mask = shard.mask;
        for (size_t at = static_cast<size_t>(tag >> 1) & mask; shard.slots[at].tag != 0; at = (at + 1) & mask) {
            const EntrySlot& slot = shard.slots[at];
            if (slot.tag != tag) continue;
            if (slot.height <= published || slot.height >= consumer_height) continue;
            const CBlock* blk = m_ring[static_cast<size_t>(slot.height) & m_ring_mask].ptr.load(std::memory_order_relaxed);
            const CTransaction& maker = *blk->vtx[slot.tx_idx];
            if (maker.GetHash() != txid) continue;
            const size_t limit = std::min(maker.vout.size(), query_tx.vout.size());
            for (uint32_t o = 0; o < static_cast<uint32_t>(limit); ++o) {
                if (!maker.vout[o].scriptPubKey.IsUnspendable()) {
                    return COutPoint{txid, o};
                }
            }
        }
        return std::nullopt;
    };

    if (num_tx == 1) {
        const CTransaction& tx = *block.vtx[0];
        const uint64_t tag = TagOf(tx.GetHash());
        const Shard& shard = m_shards[ShardOf(tag)];
        std::shared_lock lock{shard.mutex};
        return check_tx_in_shard(shard, tag, tx);
    }

    static thread_local std::vector<uint64_t> tags;
    static thread_local std::vector<uint32_t> order;
    tags.resize(num_tx);
    order.resize(num_tx);

    std::array<uint32_t, NUM_SHARDS> counts{};
    for (size_t i = 0; i < num_tx; ++i) {
        const uint64_t t = TagOf(block.vtx[i]->GetHash());
        tags[i] = t;
        ++counts[ShardOf(t)];
    }
    std::array<uint32_t, NUM_SHARDS + 1> offsets{};
    for (size_t s = 0; s < NUM_SHARDS; ++s) {
        offsets[s + 1] = offsets[s] + counts[s];
    }
    std::array<uint32_t, NUM_SHARDS> write_pos{};
    for (size_t s = 0; s < NUM_SHARDS; ++s) write_pos[s] = offsets[s];
    for (uint32_t i = 0; i < static_cast<uint32_t>(num_tx); ++i) {
        order[write_pos[ShardOf(tags[i])]++] = i;
    }

    const size_t start = m_shard_cursor.fetch_add(1, std::memory_order_relaxed) & (NUM_SHARDS - 1);
    for (size_t k = 0; k < NUM_SHARDS; ++k) {
        const size_t s = (start + k) & (NUM_SHARDS - 1);
        const uint32_t begin = offsets[s];
        const uint32_t end = offsets[s + 1];
        if (begin == end) continue;

        const Shard& shard = m_shards[s];
        std::shared_lock lock{shard.mutex};
        if (shard.count == 0) continue;

        for (uint32_t p = begin; p < end; ++p) {
            const uint32_t tx_idx = order[p];
            if (auto hit = check_tx_in_shard(shard, tags[tx_idx], *block.vtx[tx_idx])) {
                return hit;
            }
        }
    }
    return std::nullopt;
}

void WindowOutputTable::ReleaseHeights(int committed_height, std::span<const int> heights)
{
    m_published.store(committed_height, std::memory_order_release);
    if (heights.empty()) return;
    LOCK(m_released_mutex);
    m_released.insert(m_released.end(), heights.begin(), heights.end());
}

bool WindowOutputTable::EraseOne()
{
    int height = -1;
    {
        LOCK(m_released_mutex);
        if (m_released.empty()) return false;
        height = m_released.back();
        m_released.pop_back();
    }

    HeldBlock& held = m_ring[static_cast<size_t>(height) & m_ring_mask];
    const CBlock* blk = held.ptr.load(std::memory_order_acquire);
    if (!blk || held.height.load(std::memory_order_relaxed) != height) {
        return true;
    }

    const size_t num_tx = blk->vtx.size();
    static thread_local std::vector<uint64_t> tags;
    static thread_local std::vector<uint32_t> order;
    tags.resize(num_tx);
    order.resize(num_tx);

    std::array<uint32_t, NUM_SHARDS> counts{};
    for (size_t i = 0; i < num_tx; ++i) {
        const uint64_t t = TagOf(blk->vtx[i]->GetHash());
        tags[i] = t;
        ++counts[ShardOf(t)];
    }
    std::array<uint32_t, NUM_SHARDS + 1> offsets{};
    for (size_t s = 0; s < NUM_SHARDS; ++s) {
        offsets[s + 1] = offsets[s] + counts[s];
    }
    std::array<uint32_t, NUM_SHARDS> write_pos{};
    for (size_t s = 0; s < NUM_SHARDS; ++s) write_pos[s] = offsets[s];
    for (uint32_t i = 0; i < static_cast<uint32_t>(num_tx); ++i) {
        order[write_pos[ShardOf(tags[i])]++] = i;
    }

    for (size_t s = 0; s < NUM_SHARDS; ++s) {
        const uint32_t begin = offsets[s];
        const uint32_t end = offsets[s + 1];
        if (begin == end) continue;

        Shard& shard = m_shards[s];
        std::unique_lock lock{shard.mutex};
        if (shard.count == 0) continue;
        const size_t mask = shard.mask;

        for (uint32_t p = begin; p < end; ++p) {
            const uint32_t tx_idx = order[p];
            const uint64_t tag = tags[tx_idx];
            size_t at = static_cast<size_t>(tag >> 1) & mask;
            while (shard.slots[at].tag != 0) {
                const EntrySlot& cur = shard.slots[at];
                if (cur.tag == tag && cur.tx_idx == tx_idx && cur.height == height) {
                    size_t hole = at;
                    size_t next = (hole + 1) & mask;
                    while (shard.slots[next].tag != 0) {
                        const size_t home = static_cast<size_t>(shard.slots[next].tag >> 1) & mask;
                        if (((hole - home) & mask) < ((next - home) & mask)) {
                            shard.slots[hole] = shard.slots[next];
                            hole = next;
                        }
                        next = (next + 1) & mask;
                    }
                    shard.slots[hole] = EntrySlot{};
                    --shard.count;
                    break;
                }
                at = (at + 1) & mask;
            }
        }
    }

    // Release the ring slot and drop CBlock ownership outside all shard locks.
    std::shared_ptr<const CBlock> freed = std::move(held.owner);
    held.ptr.store(nullptr, std::memory_order_release);
    return true;
}

void WindowOutputTable::Sweep()
{
    while (EraseOne()) {}
}

void WindowOutputTable::Clear()
{
    {
        LOCK(m_released_mutex);
        m_released.clear();
    }
    for (size_t s = 0; s < NUM_SHARDS; ++s) {
        Shard& shard = m_shards[s];
        std::unique_lock lock{shard.mutex};
        std::vector<EntrySlot>{}.swap(shard.slots);
        shard.mask = 0;
        shard.count = 0;
        UpdateShardBudget(shard);
    }
    for (size_t i = 0; i < m_ring_size; ++i) {
        HeldBlock& held = m_ring[i];
        held.ptr.store(nullptr, std::memory_order_relaxed);
        held.height.store(-1, std::memory_order_relaxed);
        held.owner.reset();
    }
    m_published.store(-1, std::memory_order_release);
}
