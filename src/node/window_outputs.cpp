// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/window_outputs.h>

#include <util/check.h>

#include <algorithm>
#include <thread>
#include <utility>

namespace node {

namespace {

inline void PrefetchSlot(const void* ptr)
{
#if defined(__GNUC__) || defined(__clang__)
    __builtin_prefetch(ptr);
#else
    (void)ptr;
#endif
}

} // namespace

void WindowOutputs::ComputeTags(const CBlock& block, std::vector<uint64_t>& tags) const
{
    tags.resize(block.vtx.size());
    for (size_t i{0}; i < block.vtx.size(); ++i) {
        tags[i] = Tag(block.vtx[i]->GetHash());
    }
}

void WindowOutputs::Group(const std::vector<uint64_t>& tags, std::vector<uint32_t>& order, std::array<uint32_t, SHARDS + 1>& start)
{
    start.fill(0);
    for (const uint64_t tag : tags) ++start[ShardOf(tag) + 1];
    for (size_t n{0}; n < SHARDS; ++n) start[n + 1] += start[n];
    order.resize(tags.size());
    std::array<uint32_t, SHARDS> next;
    std::copy(start.begin(), start.end() - 1, next.begin());
    for (uint32_t i{0}; i < tags.size(); ++i) order[next[ShardOf(tags[i])]++] = i;
}

void WindowOutputs::Put(Shard& shard, const Slot& slot)
{
    size_t at{(slot.tag >> 1) & shard.mask};
    while (shard.slots[at].tag != 0) at = (at + 1) & shard.mask;
    shard.slots[at] = slot;
}

void WindowOutputs::Take(Shard& shard, const Slot& slot)
{
    size_t at{(slot.tag >> 1) & shard.mask};
    while (shard.slots[at].tag != slot.tag || shard.slots[at].pos != slot.pos || shard.slots[at].height != slot.height) {
        if (!Assume(shard.slots[at].tag != 0)) return;
        at = (at + 1) & shard.mask;
    }
    for (size_t next{at};;) {
        next = (next + 1) & shard.mask;
        const Slot& moved{shard.slots[next]};
        if (moved.tag == 0) break;
        const size_t home{(moved.tag >> 1) & shard.mask};
        if (((next - home) & shard.mask) < ((next - at) & shard.mask)) continue;
        shard.slots[at] = moved;
        at = next;
    }
    shard.slots[at] = Slot{};
    --shard.count;
}

void WindowOutputs::Index(int height, std::shared_ptr<const CBlock> block)
{
    Held& held{m_ring[height % RING]};
    while (held.at.load(std::memory_order_acquire) != nullptr) {
        if (!EraseOne()) std::this_thread::yield();
    }
    static thread_local std::vector<uint64_t> tags;
    static thread_local std::vector<uint32_t> order;
    std::array<uint32_t, SHARDS + 1> start;
    ComputeTags(*block, tags);
    Group(tags, order, start);
    held.height.store(height, std::memory_order_relaxed);
    held.block = std::move(block);
    held.at.store(held.block.get(), std::memory_order_release);
    for (size_t n{0}; n < SHARDS; ++n) {
        const size_t add{start[n + 1] - start[n]};
        if (add == 0) continue;
        Shard& shard{m_shards[n]};
        LOCK(shard.mutex);
        size_t cap{shard.slots.size()};
        while ((shard.count + add) * 2 > cap) cap = std::max<size_t>(16, cap * 2);
        if (cap != shard.slots.size()) {
            std::vector<Slot> old(cap);
            old.swap(shard.slots);
            shard.mask = cap - 1;
            for (const Slot& slot : old) {
                if (slot.tag != 0) Put(shard, slot);
            }
        }
        for (uint32_t k{start[n]}; k < start[n + 1]; ++k) {
            Put(shard, Slot{tags[order[k]], order[k], height});
        }
        shard.count += add;
    }
}

void WindowOutputs::Release(int height)
{
    if (height < 0) return;
    m_published.store(height, std::memory_order_release);
    LOCK(m_released_mutex);
    m_released.push_back(height);
}

void WindowOutputs::Sweep()
{
    while (EraseOne()) {}
}

bool WindowOutputs::EraseOne()
{
    int height;
    {
        LOCK(m_released_mutex);
        if (m_released.empty()) return false;
        height = m_released.front();
        m_released.pop_front();
    }
    Held& held{m_ring[height % RING]};
    const CBlock* const block{held.at.load(std::memory_order_acquire)};
    if (block == nullptr || held.height.load(std::memory_order_relaxed) != height) return true;
    static thread_local std::vector<uint64_t> tags;
    static thread_local std::vector<uint32_t> order;
    std::array<uint32_t, SHARDS + 1> start;
    ComputeTags(*block, tags);
    Group(tags, order, start);
    for (size_t n{0}; n < SHARDS; ++n) {
        if (start[n] == start[n + 1]) continue;
        Shard& shard{m_shards[n]};
        LOCK(shard.mutex);
        for (uint32_t k{start[n]}; k < start[n + 1]; ++k) {
            Take(shard, Slot{tags[order[k]], order[k], height});
        }
    }
    const std::shared_ptr<const CBlock> freed{std::move(held.block)};
    held.at.store(nullptr, std::memory_order_release);
    return true;
}

WindowOutputs::Found WindowOutputs::Probe(const Shard& shard, uint64_t tag, const Txid& txid, int published, int viewer) const
{
    Found found;
    if (shard.count == 0) return found;
    for (size_t at{(tag >> 1) & shard.mask}; shard.slots[at].tag != 0; at = (at + 1) & shard.mask) {
        const Slot& slot{shard.slots[at]};
        if (slot.tag != tag) continue;
        if (slot.height <= published || slot.height >= viewer || slot.height < found.height) continue;
        const CTransaction& tx{*m_ring[slot.height % RING].at.load(std::memory_order_relaxed)->vtx[slot.pos]};
        if (tx.GetHash() != txid) continue;
        found = {&tx, slot.height};
    }
    return found;
}

bool WindowOutputs::FindCoin(const COutPoint& outpoint, int viewer_height, std::optional<Coin>& coin) const
{
    coin.reset();
    const int published{m_published.load(std::memory_order_acquire)};
    const uint64_t tag{Tag(outpoint.hash)};
    const Shard& shard{m_shards[ShardOf(tag)]};
    LOCK(shard.mutex);
    const Found found{Probe(shard, tag, outpoint.hash, published, viewer_height)};
    if (found.maker == nullptr) return false;
    if (outpoint.n < found.maker->vout.size() && !found.maker->vout[outpoint.n].scriptPubKey.IsUnspendable()) {
        coin.emplace(found.maker->vout[outpoint.n], found.height, found.maker->IsCoinBase());
    }
    return true;
}

void WindowOutputs::FindCoins(std::vector<COutPoint>& outpoints,
                              int viewer_height,
                              const std::function<void(const COutPoint&, Coin&&)>& found) const
{
    const int published{m_published.load(std::memory_order_acquire)};
    const size_t count{outpoints.size()};
    static thread_local std::vector<uint64_t> tags;
    tags.resize(count);
    for (size_t i{0}; i < count; ++i) tags[i] = Tag(outpoints[i].hash);
    static thread_local std::vector<uint32_t> order;
    std::array<uint32_t, SHARDS + 1> start;
    Group(tags, order, start);
    static thread_local std::vector<std::pair<uint32_t, Coin>> coins;
    for (size_t n{0}; n < SHARDS; ++n) {
        if (start[n] == start[n + 1]) continue;
        const Shard& shard{m_shards[n]};
        coins.clear();
        {
            LOCK(shard.mutex);
            if (shard.count == 0) continue;
            for (uint32_t k{start[n]}; k < start[n + 1]; ++k) {
                if (k + AHEAD < start[n + 1]) {
                    PrefetchSlot(&shard.slots[(tags[order[k + AHEAD]] >> 1) & shard.mask]);
                }
                const uint32_t i{order[k]};
                const Found hit{Probe(shard, tags[i], outpoints[i].hash, published, viewer_height)};
                if (hit.maker == nullptr) continue;
                tags[i] = 0;
                const uint32_t out{outpoints[i].n};
                if (out < hit.maker->vout.size() && !hit.maker->vout[out].scriptPubKey.IsUnspendable()) {
                    coins.emplace_back(i, Coin{hit.maker->vout[out], hit.height, hit.maker->IsCoinBase()});
                }
            }
        }
        for (auto& [i, coin] : coins) found(outpoints[i], std::move(coin));
    }
    coins.clear();
    size_t left{0};
    for (size_t i{0}; i < count; ++i) {
        if (tags[i] != 0) outpoints[left++] = outpoints[i];
    }
    outpoints.resize(left);
}

void WindowOutputs::NamedIds(std::span<const CTransactionRef> txs, int viewer_height, std::vector<uint8_t>& named) const
{
    const int published{m_published.load(std::memory_order_acquire)};
    const size_t count{txs.size()};
    named.assign(count, 0);
    static thread_local std::vector<uint64_t> tags;
    tags.resize(count);
    for (size_t i{0}; i < count; ++i) tags[i] = Tag(txs[i]->GetHash());
    static thread_local std::vector<uint32_t> order;
    std::array<uint32_t, SHARDS + 1> start;
    Group(tags, order, start);
    for (size_t n{0}; n < SHARDS; ++n) {
        if (start[n] == start[n + 1]) continue;
        const Shard& shard{m_shards[n]};
        LOCK(shard.mutex);
        if (shard.count == 0) continue;
        for (uint32_t k{start[n]}; k < start[n + 1]; ++k) {
            if (k + AHEAD < start[n + 1]) {
                PrefetchSlot(&shard.slots[(tags[order[k + AHEAD]] >> 1) & shard.mask]);
            }
            const uint64_t tag{tags[order[k]]};
            for (size_t at{(tag >> 1) & shard.mask}; shard.slots[at].tag != 0; at = (at + 1) & shard.mask) {
                const Slot& slot{shard.slots[at]};
                if (slot.tag == tag && slot.height > published && slot.height < viewer_height) {
                    named[order[k]] = 1;
                    break;
                }
            }
        }
    }
}

} // namespace node
