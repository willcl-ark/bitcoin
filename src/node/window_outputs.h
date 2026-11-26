// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_NODE_WINDOW_OUTPUTS_H
#define BITCOIN_NODE_WINDOW_OUTPUTS_H

#include <block_apply_view.h>
#include <coins.h>
#include <primitives/block.h>
#include <sync.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace node {

inline constexpr int MAX_CONNECT_WINDOW{64};

/**
 * Outputs created by unpublished blocks in flight inside a parallel connect window.
 *
 * A window block's transactions are indexed here after parsing, and validation of higher blocks
 * starts only once every lower block in the window has been indexed. Lookups see outputs from
 * strictly lower unpublished blocks (`published < height < viewer_height`). Spends are not tracked
 * here; cross-block double spends are detected when committing to CoinsStore and retried serially.
 *
 * All window blocks share a sharded open-addressing hash table keyed by process-salted txid tags.
 * Published blocks are queued in FIFO order and swept out by worker threads.
 */
class WindowOutputs final : public WindowCoinSource
{
public:
    void SetPublished(int height) { m_published.store(height, std::memory_order_release); }

    //! Index all transactions of `block` at `height`.
    void Index(int height, std::shared_ptr<const CBlock> block) EXCLUSIVE_LOCKS_REQUIRED(!m_released_mutex);

    //! Mark `height` as published (its outputs now live in CoinsStore) and enqueue it for sweeping.
    void Release(int height) EXCLUSIVE_LOCKS_REQUIRED(!m_released_mutex);

    //! Sweep items of published blocks out of the table and release their CBlock references.
    void Sweep() EXCLUSIVE_LOCKS_REQUIRED(!m_released_mutex);

    bool FindCoin(const COutPoint& outpoint, int viewer_height, std::optional<Coin>& coin, uint32_t own_limit = 0) const override;
    void FindCoins(std::vector<COutPoint>& outpoints,
                   int viewer_height,
                   const std::function<void(const COutPoint&, Coin&&)>& found) const override;
    void FindInputs(std::span<const COutPoint> outpoints,
                    std::span<const uint32_t> positions,
                    int viewer_height,
                    std::vector<uint8_t>& decided,
                    std::vector<Coin>& coins) const override;
    void NamedIds(std::span<const CTransactionRef> txs, int viewer_height, std::vector<uint8_t>& named) const override;

private:
    static constexpr int RING{256};
    static_assert(RING >= MAX_CONNECT_WINDOW);
    static constexpr size_t SHARDS{1024};
    static constexpr uint32_t AHEAD{8};

    struct Slot {
        uint64_t tag{0};
        uint32_t pos{0};
        int height{0};
    };

    struct alignas(64) Shard {
        mutable Mutex mutex;
        std::vector<Slot> slots GUARDED_BY(mutex);
        size_t mask GUARDED_BY(mutex){0};
        size_t count GUARDED_BY(mutex){0};
    };

    struct Held {
        std::atomic<const CBlock*> at{nullptr};
        std::atomic<int> height{-1};
        std::shared_ptr<const CBlock> block;
    };

    struct Found {
        const CTransaction* maker{nullptr};
        int height{-1};
    };

    uint64_t Tag(const Txid& txid) const noexcept { return static_cast<uint64_t>(m_hasher(txid)) | 1; }
    static size_t ShardOf(uint64_t tag) noexcept { return tag >> 54; }

    void ComputeTags(const CBlock& block, std::vector<uint64_t>& tags) const;
    static void Group(const std::vector<uint64_t>& tags, std::vector<uint32_t>& order, std::array<uint32_t, SHARDS + 1>& start);
    static void Put(Shard& shard, const Slot& slot) EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);
    static void Take(Shard& shard, const Slot& slot) EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);
    bool EraseOne() EXCLUSIVE_LOCKS_REQUIRED(!m_released_mutex);
    Found Probe(const Shard& shard, uint64_t tag, const Txid& txid, int published, int viewer, uint32_t own_limit) const EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);

    const SaltedCoinsCacheHasher m_hasher;
    std::array<Shard, SHARDS> m_shards;
    std::array<Held, RING> m_ring;
    std::atomic<int> m_published{-1};
    Mutex m_released_mutex;
    //! FIFO queue so oldest ring slots are always reclaimed first.
    std::deque<int> m_released GUARDED_BY(m_released_mutex);
};

} // namespace node

#endif // BITCOIN_NODE_WINDOW_OUTPUTS_H
