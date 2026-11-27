// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <ibd_window.h>

#include <chain.h>
#include <core_memusage.h>
#include <crypto/common.h>
#include <memusage.h>
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
