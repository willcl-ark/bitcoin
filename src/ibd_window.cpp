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
        m_live_entry_bytes.fetch_add(add * sizeof(EntrySlot), std::memory_order_relaxed);
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

    if (m_published.load(std::memory_order_acquire) + 1 >= consumer_height) return;

    auto probe_one = [&](const Shard& shard, int published, uint64_t tag, const COutPoint& op) noexcept -> std::optional<Coin> {
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
        const int published = m_published.load(std::memory_order_acquire);
        if (published + 1 >= consumer_height) return;
        if (auto coin = probe_one(shard, published, tag, outpoints[0])) {
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
        const int published = m_published.load(std::memory_order_acquire);
        if (published + 1 >= consumer_height) return;

        for (uint32_t p = begin; p < end; ++p) {
            const uint32_t idx = order[p];
            if (auto coin = probe_one(shard, published, tags[idx], outpoints[idx])) {
                results[idx] = std::move(*coin);
            }
        }
    }
}

std::optional<COutPoint> WindowOutputTable::FindBIP30ConflictBelowHeight(int consumer_height, const CBlock& block) const
{
    const size_t num_tx = block.vtx.size();
    if (num_tx == 0) return std::nullopt;
    if (m_published.load(std::memory_order_acquire) + 1 >= consumer_height) return std::nullopt;

    auto check_tx_in_shard = [&](const Shard& shard, int published, uint64_t tag, const CTransaction& query_tx) noexcept -> std::optional<COutPoint> {
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
        const int published = m_published.load(std::memory_order_acquire);
        if (published + 1 >= consumer_height) return std::nullopt;
        return check_tx_in_shard(shard, published, tag, tx);
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
        const int published = m_published.load(std::memory_order_acquire);
        if (published + 1 >= consumer_height) return std::nullopt;

        for (uint32_t p = begin; p < end; ++p) {
            const uint32_t tx_idx = order[p];
            if (auto hit = check_tx_in_shard(shard, published, tags[tx_idx], *block.vtx[tx_idx])) {
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
        height = m_released.front();
        m_released.pop_front();
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
        size_t erased = 0;

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
                    ++erased;
                    break;
                }
                at = (at + 1) & mask;
            }
        }
        if (erased > 0) {
            m_live_entry_bytes.fetch_sub(erased * sizeof(EntrySlot), std::memory_order_relaxed);
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
        if (shard.count > 0) {
            m_live_entry_bytes.fetch_sub(shard.count * sizeof(EntrySlot), std::memory_order_relaxed);
            shard.count = 0;
        }
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

ParallelIBDWindow::Slot::Slot() = default;
ParallelIBDWindow::Slot::~Slot() = default;

ParallelIBDWindow::ParallelIBDWindow(
    ChainstateManager& chainman,
    ShardedCoinsView& coins_tip,
    const CCoinsView& coins_base,
    size_t window_width,
    size_t coinstip_budget_bytes)
    : m_chainman{chainman},
      m_coins_tip{coins_tip},
      m_coins_base{coins_base},
      m_budget{*chainman.m_ibd_budget},
      m_window_width{std::max<size_t>(window_width, 1)},
      m_caps{IBDMemoryBudget::ComputeCaps(coinstip_budget_bytes)},
      m_output_table{m_budget, m_window_width * 2 + 1}
{
}

ParallelIBDWindow::~ParallelIBDWindow()
{
    DrainOffCsMain();
}

node::SerializedBlockUndo ParallelIBDWindow::TakeSerializedUndo(Slot& slot) noexcept
{
    if (slot.budgeted_undo_bytes > 0) {
        m_budget.pending_undo_bytes.fetch_sub(slot.budgeted_undo_bytes, std::memory_order_relaxed);
        slot.budgeted_undo_bytes = 0;
    }
    return std::move(slot.serialized_undo);
}

void ParallelIBDWindow::ReleaseLoadingReservation(Slot& slot) noexcept
{
    AssertLockHeld(m_mutex);
    if (slot.reserved_loading_bytes > 0) {
        Assert(m_reserved_loading_bytes >= slot.reserved_loading_bytes);
        m_reserved_loading_bytes -= slot.reserved_loading_bytes;
        slot.reserved_loading_bytes = 0;
    }
}

void ParallelIBDWindow::ReleaseSlotBudget(Slot& slot) noexcept
{
    if (slot.budgeted_block_bytes > 0) {
        m_budget.window_blocks_bytes.fetch_sub(slot.budgeted_block_bytes, std::memory_order_relaxed);
        slot.budgeted_block_bytes = 0;
    }
    if (slot.budgeted_undo_bytes > 0) {
        m_budget.pending_undo_bytes.fetch_sub(slot.budgeted_undo_bytes, std::memory_order_relaxed);
        slot.budgeted_undo_bytes = 0;
    }
}

size_t ParallelIBDWindow::MaxSchedulableSlots() const
{
    LOCK(m_mutex);
    if (m_slots.size() >= m_window_width) return 0;
    const size_t effective_block_bytes =
        m_budget.window_blocks_bytes.load(std::memory_order_relaxed) + m_reserved_loading_bytes;
    const bool over_budget =
        effective_block_bytes >= m_caps.max_window_blocks_bytes ||
        m_output_table.LiveEntryBytes() >= m_caps.max_window_outputs_bytes ||
        m_budget.pending_undo_bytes.load(std::memory_order_relaxed) >= m_caps.max_pending_undo_bytes;
    if (over_budget) {
        // Always allow at least 1 slot when the window is empty for forward progress.
        return m_slots.empty() ? 1 : 0;
    }
    const size_t remaining = m_caps.max_window_blocks_bytes - effective_block_bytes;
    const size_t by_budget = remaining / std::max<size_t>(m_avg_block_mem_bytes, 1);
    if (by_budget == 0) {
        return m_slots.empty() ? 1 : 0;
    }
    return std::min(m_window_width - m_slots.size(), by_budget);
}

bool ParallelIBDWindow::CanScheduleMore() const
{
    return MaxSchedulableSlots() > 0;
}

size_t ParallelIBDWindow::Size() const
{
    LOCK(m_mutex);
    return m_slots.size();
}

std::optional<int> ParallelIBDWindow::HighestScheduledHeight() const
{
    LOCK(m_mutex);
    if (m_slots.empty()) return std::nullopt;
    return m_slots.back()->height;
}

void ParallelIBDWindow::SetSlotTerminalState(const std::shared_ptr<Slot>& slot, SlotState terminal_state)
{
    bool notify_front = false;
    {
        LOCK(m_mutex);
        slot->state = terminal_state;
        notify_front = !m_slots.empty() && m_slots.front() == slot;
    }
    if (notify_front) m_cv.notify_one();
}

void ParallelIBDWindow::ScheduleSlot(std::shared_ptr<Slot> slot)
{
    {
        LOCK(m_mutex);
        const size_t est = m_avg_block_mem_bytes;
        slot->reserved_loading_bytes = est;
        m_reserved_loading_bytes += est;
        slot->state = SlotState::LOADING;
        m_slots.push_back(slot);
        ++m_inflight_tasks;
    }

    auto pool = m_chainman.GetValidationPool();
    auto res = pool->Submit([this, slot]() mutable {
        RunLoadAndIndexTask(std::move(slot));
    });
    if (!res) {
        // Pool inactive or interrupted: mark slot failed cleanly so caller falls back to serial.
        bool notify_front = false;
        {
            LOCK(m_mutex);
            ReleaseLoadingReservation(*slot);
            slot->state = SlotState::FAILED_VALIDATION;
            notify_front = !m_slots.empty() && m_slots.front() == slot;
            if (--m_inflight_tasks == 0) m_drained_cv.notify_all();
        }
        if (notify_front) m_cv.notify_one();
    }
}

void ParallelIBDWindow::ScheduleSlots(std::vector<std::shared_ptr<Slot>>&& slots)
{
    if (slots.empty()) return;
    if (slots.size() == 1) {
        ScheduleSlot(std::move(slots[0]));
        return;
    }

    const size_t n = slots.size();
    // Coalesce asynchronous NVMe readahead hints per blk*.dat file purely in memory here,
    // then run BlockManager::AdviseBlockRange() on a dedicated pool task so neither the
    // coordinator nor slots[0] blocks in page-cache syscalls.
    struct AdviseRange {
        FlatFilePos pos;
        uint64_t start;
        uint64_t len;
    };
    std::vector<AdviseRange> advise_ranges;
    {
        size_t i = 0;
        while (i < slots.size()) {
            const FlatFilePos& p0 = slots[i]->disk_pos;
            if (p0.IsNull() || p0.nPos < node::STORAGE_HEADER_BYTES) {
                ++i;
                continue;
            }
            const int fnum = p0.nFile;
            uint64_t range_start = p0.nPos - node::STORAGE_HEADER_BYTES;
            uint64_t range_end = p0.nPos;
            size_t j = i + 1;
            while (j < slots.size()) {
                const FlatFilePos& pj = slots[j]->disk_pos;
                if (pj.nFile != fnum || pj.nPos < range_start || pj.nPos > range_end + 8_MiB) break;
                if (pj.nPos > range_end) range_end = pj.nPos;
                ++j;
            }
            advise_ranges.push_back(AdviseRange{p0, range_start, (range_end - range_start) + 4_MiB});
            i = j;
        }
    }
    const size_t extra_tasks = advise_ranges.empty() ? 0 : 1;

    {
        LOCK(m_mutex);
        const size_t est = m_avg_block_mem_bytes;
        for (auto& slot : slots) {
            slot->reserved_loading_bytes = est;
            m_reserved_loading_bytes += est;
            slot->state = SlotState::LOADING;
            m_slots.push_back(slot);
        }
        m_inflight_tasks += n + extra_tasks;
    }

    std::vector<std::function<void()>> tasks;
    tasks.reserve(n + extra_tasks);
    if (!advise_ranges.empty()) {
        auto ranges = std::make_shared<std::vector<AdviseRange>>(std::move(advise_ranges));
        tasks.emplace_back([this, ranges = std::move(ranges)]() mutable {
            struct AdviseGuard {
                ParallelIBDWindow& w;
                std::shared_ptr<std::vector<AdviseRange>>& r;
                ~AdviseGuard()
                {
                    r.reset();
                    LOCK(w.m_mutex);
                    if (--w.m_inflight_tasks == 0) w.m_drained_cv.notify_all();
                }
            } guard{*this, ranges};
            if (!m_cancelled.load(std::memory_order_acquire)) {
                for (const auto& r : *ranges) {
                    m_chainman.m_blockman.AdviseBlockRange(r.pos, r.start, r.len);
                }
            }
        });
    }
    for (const auto& slot : slots) {
        tasks.emplace_back([this, s = slot]() mutable {
            RunLoadAndIndexTask(std::move(s));
        });
    }

    auto pool = m_chainman.GetValidationPool();
    auto res = pool->Submit(std::move(tasks));
    if (!res) {
        bool notify_front = false;
        {
            LOCK(m_mutex);
            for (auto& slot : slots) {
                ReleaseLoadingReservation(*slot);
                slot->state = SlotState::FAILED_VALIDATION;
                if (!m_slots.empty() && m_slots.front() == slot) {
                    notify_front = true;
                }
            }
            m_inflight_tasks -= (n + extra_tasks);
            if (m_inflight_tasks == 0) m_drained_cv.notify_all();
        }
        if (notify_front) m_cv.notify_one();
        return;
    }
}

void ParallelIBDWindow::RunLoadAndIndexTask(std::shared_ptr<Slot> slot)
{
    struct TaskGuard {
        ParallelIBDWindow& w;
        const std::shared_ptr<Slot>& s;
        ~TaskGuard()
        {
            LOCK(w.m_mutex);
            w.ReleaseLoadingReservation(*s);
            if (s->state == SlotState::LOADING) {
                s->state = SlotState::FAILED_VALIDATION;
                if (!w.m_slots.empty() && w.m_slots.front() == s) {
                    w.m_cv.notify_one();
                }
            }
            if (--w.m_inflight_tasks == 0) w.m_drained_cv.notify_all();
        }
    } guard{*this, slot};

    if (m_cancelled.load(std::memory_order_acquire)) return;

    // Stage 1: Check P2P handoff cache first; otherwise read from BlockManager's concurrent
    // lock-free reader and deserialize/check PoW+hash across worker cores.
    std::shared_ptr<const CBlock> block = m_chainman.m_handoff_cache.Take(slot->block_hash);
    if (!block) {
        auto loaded = std::make_shared<CBlock>();
        if (!m_chainman.m_blockman.ReadBlockConcurrent(*loaded, slot->disk_pos, slot->block_hash)) {
            if (!m_cancelled.load(std::memory_order_acquire)) {
                SetSlotTerminalState(slot, SlotState::FAILED_VALIDATION);
            }
            return;
        }
        block = std::move(loaded);
    }

    if (m_cancelled.load(std::memory_order_acquire)) return;

    slot->block = std::move(block);
    const size_t blk_bytes = EstimateBlockMemoryUsage(*slot->block);
    slot->block_mem_bytes = blk_bytes;
    slot->budgeted_block_bytes = blk_bytes;
    m_budget.window_blocks_bytes.fetch_add(blk_bytes, std::memory_order_relaxed);
    {
        LOCK(m_mutex);
        ReleaseLoadingReservation(*slot);
        m_avg_block_mem_bytes = std::clamp(
            (m_avg_block_mem_bytes * 7 + blk_bytes) / 8,
            size_t{256 * 1024},
            size_t{4_MiB});
    }

    // Run context-independent block checks (including duplicate txid detection) before
    // inserting outputs into WindowOutputTable so malformed blocks never pollute later lookups.
    if (slot->val_ctx && slot->val_ctx->run_check_block) {
        if (!CheckBlock(*slot->block, slot->val_state, *Assert(slot->val_ctx->consensus_params),
                        slot->val_ctx->check_pow_and_merkle, slot->val_ctx->check_pow_and_merkle)) {
            if (!m_cancelled.load(std::memory_order_acquire)) {
                SetSlotTerminalState(slot, SlotState::FAILED_VALIDATION);
            }
            return;
        }
    }

    // Stage 2: Index block transactions into zero-copy open-addressed WindowOutputTable.
    m_output_table.InsertBlockOutputs(slot->height, slot->block);

    std::vector<std::shared_ptr<Slot>> to_validate;
    {
        LOCK(m_mutex);
        if (m_cancelled.load(std::memory_order_acquire)) return;
        slot->state = SlotState::INDEXED;
        to_validate = CollectReadyForValidation();
    }
    if (!to_validate.empty()) {
        // Execute the earliest newly-unblocked slot (typically `slot` itself, still hot in L1/L2)
        // inline on this worker thread, and batch-submit any additional unblocked slots.
        std::shared_ptr<Slot> inline_slot = std::move(to_validate.front());
        if (to_validate.size() > 1) {
            to_validate.erase(to_validate.begin());
            DispatchValidationSlots(std::move(to_validate));
        }
        RunValidateTask(std::move(inline_slot));
    }
}

std::vector<std::shared_ptr<ParallelIBDWindow::Slot>> ParallelIBDWindow::CollectReadyForValidation()
{
    AssertLockHeld(m_mutex);
    std::vector<std::shared_ptr<Slot>> ready;
    if (m_cancelled.load(std::memory_order_acquire)) return ready;

    // Advance watermark strictly over contiguous slots that have completed Stage 2 without failing.
    for (const auto& s : m_slots) {
        if (s->state == SlotState::INDEXED || s->state == SlotState::VALIDATING || s->state == SlotState::READY) {
            if (s->height > m_indexed_watermark) {
                m_indexed_watermark = s->height;
            }
        } else {
            break;
        }
    }

    for (auto& s : m_slots) {
        if (s->height > m_indexed_watermark) break;
        if (s->state != SlotState::INDEXED) continue;

        s->state = SlotState::VALIDATING;
        ++m_inflight_tasks;
        ready.push_back(s);
    }
    return ready;
}

void ParallelIBDWindow::DispatchValidationSlots(std::vector<std::shared_ptr<Slot>>&& slots)
{
    if (slots.empty()) return;
    auto pool = m_chainman.GetValidationPool();
    if (slots.size() == 1) {
        auto& s = slots[0];
        auto res = pool->Submit([this, slot = s]() mutable {
            RunValidateTask(std::move(slot));
        });
        if (!res) {
            bool notify_front = false;
            {
                LOCK(m_mutex);
                s->state = SlotState::FAILED_VALIDATION;
                notify_front = !m_slots.empty() && m_slots.front() == s;
                if (--m_inflight_tasks == 0) m_drained_cv.notify_all();
            }
            if (notify_front) m_cv.notify_one();
        }
        return;
    }

    const size_t n = slots.size();
    std::vector<std::function<void()>> tasks;
    tasks.reserve(n);
    for (const auto& s : slots) {
        tasks.emplace_back([this, slot = s]() mutable {
            RunValidateTask(std::move(slot));
        });
    }
    auto res = pool->Submit(std::move(tasks));
    if (!res) {
        bool notify_front = false;
        {
            LOCK(m_mutex);
            for (auto& s : slots) {
                s->state = SlotState::FAILED_VALIDATION;
                if (!m_slots.empty() && m_slots.front() == s) {
                    notify_front = true;
                }
            }
            m_inflight_tasks -= n;
            if (m_inflight_tasks == 0) m_drained_cv.notify_all();
        }
        if (notify_front) m_cv.notify_one();
    }
}

void ParallelIBDWindow::RunValidateTask(std::shared_ptr<Slot> slot)
{
    struct TaskGuard {
        ParallelIBDWindow& w;
        const std::shared_ptr<Slot>& s;
        ~TaskGuard()
        {
            LOCK(w.m_mutex);
            if (s->state == SlotState::VALIDATING) {
                s->state = SlotState::FAILED_VALIDATION;
                if (!w.m_slots.empty() && w.m_slots.front() == s) {
                    w.m_cv.notify_one();
                }
            }
            if (--w.m_inflight_tasks == 0) w.m_drained_cv.notify_all();
        }
    } guard{*this, slot};

    if (m_cancelled.load(std::memory_order_acquire)) return;

    const CBlock& block = *Assert(slot->block);
    const BlockValidationContext& ctx = *Assert(slot->val_ctx);

    // Record flush epoch BEFORE resolving inputs so CommitBlockBatch can prove DB stability.
    slot->validated_flush_epoch = m_coins_tip.FlushEpoch();

    // Build flat spend-order input layout and detect intra-block self-spends.
    std::vector<uint32_t> tx_input_offsets(block.vtx.size() + 1, 0);
    size_t total_non_cb_inputs = 0;
    for (size_t i = 1; i < block.vtx.size(); ++i) {
        tx_input_offsets[i] = static_cast<uint32_t>(total_non_cb_inputs);
        total_non_cb_inputs += block.vtx[i]->vin.size();
    }
    tx_input_offsets[block.vtx.size()] = static_cast<uint32_t>(total_non_cb_inputs);

    std::vector<Coin> resolved_inputs(total_non_cb_inputs);

    struct ExternalInputRef {
        uint64_t hash_prefix{0};
        uint64_t txid_tag{0};
        COutPoint outpoint;
        uint32_t flat_idx{0};
    };
    std::vector<ExternalInputRef> ext_refs;
    ext_refs.reserve(total_non_cb_inputs);

    // Zero-allocation thread-local open-addressed table for intra-block earlier txid lookup.
    struct EarlierTxSlot {
        uint64_t tag{0};
        uint32_t tx_idx{0};
        uint32_t gen{0};
    };
    static thread_local std::vector<EarlierTxSlot> earlier_table;
    static thread_local uint32_t earlier_gen = 0;

    size_t table_cap = 64;
    while (table_cap < block.vtx.size() * 2) table_cap <<= 1;
    if (earlier_table.size() < table_cap || ++earlier_gen == 0) {
        earlier_table.assign(table_cap, EarlierTxSlot{});
        earlier_gen = 1;
    }
    const size_t earlier_mask = earlier_table.size() - 1;
    const uint32_t cur_gen = earlier_gen;

    auto insert_earlier = [&](const Txid& txid, uint32_t tx_idx) noexcept {
        const uint64_t tag = m_output_table.HashTxidTag(txid);
        size_t pos = static_cast<size_t>(tag >> 1) & earlier_mask;
        while (earlier_table[pos].gen == cur_gen) {
            pos = (pos + 1) & earlier_mask;
        }
        earlier_table[pos] = EarlierTxSlot{tag, tx_idx, cur_gen};
    };
    auto find_earlier = [&](const Txid& txid, uint64_t tag) noexcept -> int64_t {
        size_t pos = static_cast<size_t>(tag >> 1) & earlier_mask;
        while (earlier_table[pos].gen == cur_gen) {
            const EarlierTxSlot& s = earlier_table[pos];
            if (s.tag == tag && block.vtx[s.tx_idx]->GetHash() == txid) {
                return static_cast<int64_t>(s.tx_idx);
            }
            pos = (pos + 1) & earlier_mask;
        }
        return -1;
    };

    std::unordered_set<COutPoint, SaltedCoinsCacheHasher> intra_spent;

    if (!block.vtx.empty()) {
        insert_earlier(block.vtx[0]->GetHash(), 0u);
    }

    for (size_t i = 1; i < block.vtx.size(); ++i) {
        const CTransaction& tx = *block.vtx[i];
        const uint32_t base_off = tx_input_offsets[i];
        for (size_t j = 0; j < tx.vin.size(); ++j) {
            const COutPoint& prevout = tx.vin[j].prevout;
            const uint32_t flat_idx = base_off + static_cast<uint32_t>(j);
            const uint64_t tag = m_output_table.HashTxidTag(prevout.hash);
            const int64_t creator_idx = find_earlier(prevout.hash, tag);
            if (creator_idx >= 0) {
                const CTransaction& creator_tx = *block.vtx[static_cast<size_t>(creator_idx)];
                if (prevout.n < creator_tx.vout.size() &&
                    !creator_tx.vout[prevout.n].scriptPubKey.IsUnspendable() &&
                    intra_spent.insert(prevout).second) {
                    resolved_inputs[flat_idx] = Coin{creator_tx.vout[prevout.n], slot->height, creator_idx == 0};
                }
            } else {
                ext_refs.push_back(ExternalInputRef{ReadBE64(prevout.hash.data()), tag, prevout, flat_idx});
            }
        }
        insert_earlier(tx.GetHash(), static_cast<uint32_t>(i));
    }

    // Sort external inputs by big-endian txid prefix (top byte == ShardIndex 0..255),
    // tie-breaking by remaining hash bytes, vout, and flat_idx:
    // 1. Single-instruction integer compare for >99.9999% of comparisons.
    // 2. Output `unique_ops` is already grouped by ShardIndex 0..255 and ordered for LevelDB.
    // 3. Intra-block duplicate spends of an external outpoint are detected in one linear scan.
    std::sort(ext_refs.begin(), ext_refs.end(), [](const ExternalInputRef& a, const ExternalInputRef& b) noexcept {
        if (a.hash_prefix != b.hash_prefix) return a.hash_prefix < b.hash_prefix;
        const int cmp = std::memcmp(a.outpoint.hash.data() + 8, b.outpoint.hash.data() + 8, 24);
        if (cmp != 0) return cmp < 0;
        if (a.outpoint.n != b.outpoint.n) return a.outpoint.n < b.outpoint.n;
        return a.flat_idx < b.flat_idx;
    });

    std::vector<COutPoint> unique_ops;
    std::vector<uint64_t> unique_tags;
    std::vector<uint32_t> unique_flat_idxs;
    unique_ops.reserve(ext_refs.size());
    unique_tags.reserve(ext_refs.size());
    unique_flat_idxs.reserve(ext_refs.size());
    for (size_t k = 0; k < ext_refs.size(); ++k) {
        if (k > 0 && ext_refs[k].outpoint == ext_refs[k - 1].outpoint) {
            // Second spend of the same external outpoint in this block: leave spent/missing.
            continue;
        }
        unique_ops.push_back(ext_refs[k].outpoint);
        unique_tags.push_back(ext_refs[k].txid_tag);
        unique_flat_idxs.push_back(ext_refs[k].flat_idx);
    }

    std::vector<bool> from_db_flags(unique_ops.size(), false);

    if (!unique_ops.empty()) {
        // Layer 1: In-window output table (strictly lower heights).
        std::vector<std::optional<Coin>> window_hits(unique_ops.size());
        m_output_table.LookupBatch(slot->height, unique_ops, window_hits, unique_tags);

        std::vector<COutPoint> cache_ops;
        std::vector<size_t> cache_to_unique;
        cache_ops.reserve(unique_ops.size());
        cache_to_unique.reserve(unique_ops.size());

        for (size_t u = 0; u < unique_ops.size(); ++u) {
            if (window_hits[u].has_value()) {
                resolved_inputs[unique_flat_idxs[u]] = std::move(*window_hits[u]);
            } else {
                cache_ops.push_back(unique_ops[u]);
                cache_to_unique.push_back(u);
            }
        }

        // Layer 2: Concurrent sharded UTXO cache.
        if (!cache_ops.empty()) {
            std::vector<ShardedCoinsView::BatchLookupResult> cache_results(cache_ops.size());
            m_coins_tip.LookupBatch(cache_ops, cache_results);

            // Layer 3: LevelDB misses (already sorted in LevelDB key order!).
            for (size_t c = 0; c < cache_ops.size(); ++c) {
                const size_t u = cache_to_unique[c];
                switch (cache_results[c].status) {
                case ShardedCoinsView::CacheLookupStatus::LIVE:
                    resolved_inputs[unique_flat_idxs[u]] = std::move(cache_results[c].coin);
                    break;
                case ShardedCoinsView::CacheLookupStatus::TOMBSTONE:
                    // Known spent in cache: leave empty/spent.
                    break;
                case ShardedCoinsView::CacheLookupStatus::ABSENT: {
                    if (m_cancelled.load(std::memory_order_acquire)) return;
                    if (auto db_coin = m_coins_base.PeekCoin(cache_ops[c])) {
                        resolved_inputs[unique_flat_idxs[u]] = std::move(*db_coin);
                        from_db_flags[u] = true;
                    }
                    break;
                }
                }
            }
        }
    }

    if (m_cancelled.load(std::memory_order_acquire)) return;

    // Batched 3-layer BIP30 existence check (WindowOutputTable -> ShardedCoinsView -> LevelDB).
    std::vector<COutPoint> bip30_conflicts;
    if (ctx.enforce_bip30) {
        if (auto win_hit = m_output_table.FindBIP30ConflictBelowHeight(slot->height, block)) {
            bip30_conflicts.push_back(*win_hit);
        } else {
            size_t total_vouts = 0;
            for (const auto& tx : block.vtx) total_vouts += tx->vout.size();
            std::vector<COutPoint> bip30_ops;
            bip30_ops.reserve(total_vouts);
            for (const auto& tx : block.vtx) {
                const Txid& txid = tx->GetHash();
                for (uint32_t o = 0; o < static_cast<uint32_t>(tx->vout.size()); ++o) {
                    bip30_ops.emplace_back(txid, o);
                }
            }
            if (!bip30_ops.empty()) {
                std::vector<ShardedCoinsView::CacheLookupStatus> cache_statuses(
                    bip30_ops.size(), ShardedCoinsView::CacheLookupStatus::ABSENT);
                m_coins_tip.PeekCacheStatusBatch(bip30_ops, cache_statuses);
                for (size_t k = 0; k < bip30_ops.size(); ++k) {
                    if (cache_statuses[k] == ShardedCoinsView::CacheLookupStatus::LIVE) {
                        bip30_conflicts.push_back(bip30_ops[k]);
                        break;
                    }
                }
                if (bip30_conflicts.empty()) {
                    for (size_t k = 0; k < bip30_ops.size(); ++k) {
                        if (cache_statuses[k] == ShardedCoinsView::CacheLookupStatus::ABSENT) {
                            if (m_cancelled.load(std::memory_order_acquire)) return;
                            if (m_coins_base.HaveCoin(bip30_ops[k])) {
                                bip30_conflicts.push_back(bip30_ops[k]);
                                break;
                            }
                        }
                    }
                }
            }
        }
    }

    std::function<bool(const COutPoint&)> bip30_have_coin;
    if (!bip30_conflicts.empty()) {
        bip30_have_coin = [&bip30_conflicts](const COutPoint& op) -> bool {
            return std::find(bip30_conflicts.begin(), bip30_conflicts.end(), op) != bip30_conflicts.end();
        };
    }

    // Capture expected height & coinbase flag for every external spend BEFORE
    // ValidateBlockWithFlatInputs moves coins out of `resolved_inputs` into `val_result.blockundo`.
    std::vector<uint32_t> expected_heights(unique_ops.size(), 0);
    std::vector<uint32_t> expected_coinbases(unique_ops.size(), 0);
    for (size_t u = 0; u < unique_ops.size(); ++u) {
        const Coin& coin = resolved_inputs[unique_flat_idxs[u]];
        if (!coin.IsSpent()) {
            expected_heights[u] = coin.nHeight;
            expected_coinbases[u] = coin.fCoinBase ? 1u : 0u;
        }
    }

    // Execute shared decoupled block validation core off cs_main.
    BlockValidationResultData val_result;
    const bool valid = ValidateBlockWithFlatInputs(
        block, ctx, resolved_inputs, tx_input_offsets, bip30_have_coin, slot->val_state, val_result);

    if (!valid || m_cancelled.load(std::memory_order_acquire)) {
        SetSlotTerminalState(slot, SlotState::FAILED_VALIDATION);
        return;
    }

    // Stage 4: Immediately serialize undo payload in a single pass when needed
    // (skipped on -reindex-chainstate when rev*.dat already holds this block's undo)
    // and drop CBlockUndo.
    if (slot->needs_undo) {
        slot->serialized_undo = m_chainman.m_blockman.SerializeBlockUndoPayload(val_result.blockundo, slot->prev_block_hash);
        slot->budgeted_undo_bytes = slot->serialized_undo.data.capacity();
        m_budget.pending_undo_bytes.fetch_add(slot->budgeted_undo_bytes, std::memory_order_relaxed);
    }
    val_result.blockundo.vtxundo.clear();
    val_result.blockundo.vtxundo.shrink_to_fit();

    // Build pre-sharded commit summary (net external spends + net created outputs) on worker core
    // so CommitBlockBatch can apply shards in parallel with zero coordinator-thread op copying.
    // Note: `unique_ops` is already sorted in LevelDB key order (hence already ordered by ShardIndex 0..255).
    slot->spends.reserve(unique_ops.size());
    std::array<uint32_t, ShardedCoinsView::NUM_SHARDS> spend_counts{};
    for (size_t u = 0; u < unique_ops.size(); ++u) {
        ++spend_counts[ShardedCoinsView::ShardIndex(unique_ops[u])];
        slot->spends.push_back(SpendOp{
            unique_ops[u],
            expected_heights[u],
            expected_coinbases[u],
            from_db_flags[u],
        });
    }
    slot->spend_shard_offsets[0] = 0;
    for (size_t s = 0; s < ShardedCoinsView::NUM_SHARDS; ++s) {
        slot->spend_shard_offsets[s + 1] = slot->spend_shard_offsets[s] + spend_counts[s];
    }

    std::array<uint32_t, ShardedCoinsView::NUM_SHARDS> create_counts{};
    size_t total_creates = 0;
    for (const auto& tx : block.vtx) {
        const Txid& txid = tx->GetHash();
        const size_t s = std::to_integer<size_t>(txid.data()[0]);
        for (uint32_t o = 0; o < static_cast<uint32_t>(tx->vout.size()); ++o) {
            if (tx->vout[o].scriptPubKey.IsUnspendable()) continue;
            if (!intra_spent.empty() && intra_spent.contains(COutPoint{txid, o})) continue;
            ++create_counts[s];
            ++total_creates;
        }
    }
    slot->create_shard_offsets[0] = 0;
    for (size_t s = 0; s < ShardedCoinsView::NUM_SHARDS; ++s) {
        slot->create_shard_offsets[s + 1] = slot->create_shard_offsets[s] + create_counts[s];
    }
    slot->creates.resize(total_creates);
    std::array<uint32_t, ShardedCoinsView::NUM_SHARDS> create_write_pos{};
    for (size_t s = 0; s < ShardedCoinsView::NUM_SHARDS; ++s) {
        create_write_pos[s] = slot->create_shard_offsets[s];
    }
    Assert(slot->height >= 0 && static_cast<uint32_t>(slot->height) <= ShardedCoinsView::MAX_HEIGHT);
    for (const auto& tx : block.vtx) {
        const bool is_coinbase = tx->IsCoinBase();
        const bool possible_overwrite = is_coinbase && !ctx.enforce_bip30;
        const Txid& txid = tx->GetHash();
        const size_t s = std::to_integer<size_t>(txid.data()[0]);
        for (uint32_t o = 0; o < static_cast<uint32_t>(tx->vout.size()); ++o) {
            if (tx->vout[o].scriptPubKey.IsUnspendable()) continue;
            if (!intra_spent.empty() && intra_spent.contains(COutPoint{txid, o})) continue;
            slot->creates[create_write_pos[s]++] = CreateOp{
                tx.get(),
                o,
                static_cast<uint8_t>(is_coinbase),
                static_cast<uint8_t>(possible_overwrite),
            };
        }
    }

    const size_t commit_ops_bytes = slot->spends.capacity() * sizeof(SpendOp) + slot->creates.capacity() * sizeof(CreateOp);
    slot->budgeted_block_bytes += commit_ops_bytes;
    m_budget.window_blocks_bytes.fetch_add(commit_ops_bytes, std::memory_order_relaxed);

    slot->inputs_count = val_result.inputs_count;
    slot->sigops_cost = val_result.sigops_cost;
    slot->time_connect = val_result.time_connect;
    slot->time_verify = val_result.time_verify;

    SetSlotTerminalState(slot, SlotState::READY);

    // Sweep any committed heights out of WindowOutputTable off the coordinator thread
    // AFTER publishing READY so this block's critical path is never delayed.
    if (!m_cancelled.load(std::memory_order_acquire)) {
        m_output_table.Sweep();
    }
}

bool ParallelIBDWindow::WaitForFrontReady(const std::function<bool()>& should_stop)
{
    AssertLockNotHeld(::cs_main);
    WAIT_LOCK(m_mutex, lock);
    m_cv.wait(lock, [&]() EXCLUSIVE_LOCKS_REQUIRED(m_mutex) {
        if (should_stop && should_stop()) return true;
        if (m_slots.empty()) return true;
        const SlotState st = m_slots.front()->state;
        return st == SlotState::READY || st == SlotState::FAILED_VALIDATION;
    });
    if (m_slots.empty()) return false;
    const SlotState st = m_slots.front()->state;
    return st == SlotState::READY || st == SlotState::FAILED_VALIDATION;
}

bool ParallelIBDWindow::IsFrontFailed() const
{
    LOCK(m_mutex);
    return !m_slots.empty() && m_slots.front()->state == SlotState::FAILED_VALIDATION;
}

std::vector<std::shared_ptr<ParallelIBDWindow::Slot>> ParallelIBDWindow::PeekReadyPrefix(size_t max_batch) const
{
    LOCK(m_mutex);
    std::vector<std::shared_ptr<Slot>> prefix;
    prefix.reserve(std::min(max_batch, m_slots.size()));
    for (const auto& s : m_slots) {
        if (prefix.size() >= max_batch || s->state != SlotState::READY) break;
        prefix.push_back(s);
    }
    return prefix;
}

void ParallelIBDWindow::ReleaseCommittedPrefix(size_t count, int new_tip_height)
{
    AssertLockNotHeld(::cs_main);
    auto released = std::make_shared<std::vector<std::shared_ptr<Slot>>>();
    released->reserve(count);
    std::vector<int> released_heights;
    released_heights.reserve(count);
    {
        LOCK(m_mutex);
        const size_t to_pop = std::min(count, m_slots.size());
        for (size_t i = 0; i < to_pop; ++i) {
            ReleaseLoadingReservation(*m_slots.front());
            released_heights.push_back(m_slots.front()->height);
            released->push_back(std::move(m_slots.front()));
            m_slots.pop_front();
        }
    }
    for (auto& slot : *released) {
        ReleaseSlotBudget(*slot);
    }
    // O(batch_size) coordinator release; worker threads sweep shards and free CBlock copies.
    m_output_table.ReleaseHeights(new_tip_height, released_heights);
    // Offload freeing released Slot vectors / CBlock refs off the coordinator thread.
    if (!released->empty()) {
        auto pool = m_chainman.GetValidationPool();
        (void)pool->Submit([r = std::move(released)]() mutable { r.reset(); });
    }
}

void ParallelIBDWindow::DrainOffCsMain()
{
    AssertLockNotHeld(::cs_main);
    m_cancelled.store(true, std::memory_order_release);
    m_cv.notify_all();

    std::deque<std::shared_ptr<Slot>> leftover;
    {
        WAIT_LOCK(m_mutex, lock);
        m_drained_cv.wait(lock, [&]() EXCLUSIVE_LOCKS_REQUIRED(m_mutex) {
            return m_inflight_tasks == 0;
        });
        for (auto& slot : m_slots) {
            ReleaseLoadingReservation(*slot);
        }
        Assume(m_reserved_loading_bytes == 0);
        m_reserved_loading_bytes = 0;
        leftover.swap(m_slots);
        m_indexed_watermark = -1;
    }

    for (auto& slot : leftover) {
        ReleaseSlotBudget(*slot);
    }
    leftover.clear();
    m_output_table.Clear();
    m_chainman.m_blockman.CloseConcurrentReadHandles();
}
