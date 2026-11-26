// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_NODE_CONNECT_WINDOW_H
#define BITCOIN_NODE_CONNECT_WINDOW_H

#include <chain.h>
#include <coinsstore.h>
#include <consensus/validation.h>
#include <flatfile.h>
#include <node/window_outputs.h>
#include <primitives/block.h>
#include <sync.h>
#include <undo.h>
#include <util/byte_units.h>
#include <util/time.h>

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <future>
#include <limits>
#include <memory>
#include <span>
#include <utility>
#include <vector>

struct BlockValidationContext;

namespace node {

inline constexpr size_t COMMIT_GROUPS{64};
inline constexpr size_t MAX_COMMIT_BLOCKS{32};
inline constexpr size_t MAX_COMMIT_BLOCKS_LARGE{8};
inline constexpr size_t COMMIT_ITEMS_PER_HELPER{256};
inline constexpr size_t COMMIT_WORKERS_PER_HELPER{4};
inline constexpr size_t MAX_COMMIT_HELPERS{16};

inline constexpr size_t WINDOW_BYTES_SHARE{16};
inline constexpr size_t MIN_WINDOW_BYTES{4_MiB};
inline constexpr size_t MAX_WINDOW_BYTES{256_MiB};
inline constexpr size_t MIN_PINNED_BYTES{4_MiB};
inline constexpr size_t MAX_PINNED_BYTES{256_MiB};
inline constexpr size_t MIN_UNDO_BACKLOG_BYTES{8_MiB};
inline constexpr size_t MAX_UNDO_BACKLOG_BYTES{64_MiB};
inline constexpr size_t WINDOW_BYTES_PER_ITEM{512};
inline constexpr size_t RELEASED_SWEEP_THRESHOLD{8};

//! Compact 16-byte UTXO mutation prepared by a window worker from a validated block and its undo data.
struct CommitItem {
    static constexpr uint8_t SPEND{1U << 0};
    //! Create that may overwrite an existing unspent coin (coinbase / BIP30).
    static constexpr uint8_t OVERWRITE{1U << 1};
    //! Spend whose input coin was read directly from the backing DB.
    static constexpr uint8_t FROM_BASE{1U << 2};

    //! For SPEND: `const COutPoint*` into the block's txin. For create: `const CTransaction*`.
    const void* at{nullptr};
    //! For SPEND: `(nHeight << 1) | fCoinBase`. For create: vout index `n`.
    uint32_t n{0};
    uint8_t flags{0};

    bool Spend() const noexcept { return (flags & SPEND) != 0; }
    const CTransaction& Tx() const noexcept { return *static_cast<const CTransaction*>(at); }
    COutPoint Outpoint() const noexcept
    {
        if (Spend()) return *static_cast<const COutPoint*>(at);
        return COutPoint{Tx().GetHash(), n};
    }
};

enum class WindowBlockState : uint8_t {
    Running,
    Indexed,
    Validating,
    Complete,
    Failed,
};

inline bool IsWindowIndexed(WindowBlockState s)
{
    return s == WindowBlockState::Indexed || s == WindowBlockState::Validating || s == WindowBlockState::Complete;
}

inline bool IsWindowComplete(WindowBlockState s)
{
    return s == WindowBlockState::Complete || s == WindowBlockState::Failed;
}

struct WindowSync;
struct CommitJob;

struct StagedConnect {
    CBlockIndex* pindex{nullptr};
    FlatFilePos block_pos;
    CoinsStore* store{nullptr};
    const char* script_check_reason{nullptr};
    bool write_undo{true};

    std::shared_ptr<const CBlock> block;
    std::shared_ptr<BlockValidationContext> ctx;
    bool read_failed{false};
    bool ok{false};
    BlockValidationState state;
    std::vector<unsigned char> serialized_undo;
    int num_inputs{0};
    int64_t sigops_cost{0};
    bool failed_in_check_block{false};

    std::future<void> done;
    std::shared_ptr<WindowSync> sync;
    WindowBlockState window{WindowBlockState::Running};

    size_t bytes{0};
    size_t staged_bytes{0};
    size_t parsed_bytes{0};
    SteadyClock::time_point time_start{};
    SteadyClock::time_point time_read{};
    SteadyClock::time_point time_connected{};

    uint64_t resolve_flush_gen{0};
    std::vector<uint32_t> group_starts;
    std::vector<CommitItem> items;
    bool committed{false};
    bool commit_failed{false};
    bool in_job{false};
};

/**
 * 2-phase parallel commit job over contiguous validated blocks:
 * - Phase 1 (`CheckShard`): read-only verification of every shard group (no CoinsStore mutations).
 * - Phase 2 (`ApplyShardUnconditional`): unconditional apply of verified prefix `0..valid_blocks-1`.
 *
 * Because Phase 1 mutates nothing and Phase 2 applies only verified blocks, conflicts and DB
 * read errors require zero rollback.
 */
struct CommitJob {
    static constexpr uint32_t NO_CONFLICT{std::numeric_limits<uint32_t>::max()};

    CoinsStore* store{nullptr};
    size_t num_groups{0};
    size_t items{0};
    std::vector<StagedConnect*> blocks;
    std::vector<int> heights;
    uint64_t store_flush_gen{0};

    struct GroupData {
        size_t shard_begin{0};
        size_t shard_end{0};
        std::vector<uint32_t> shard_starts;
        std::vector<CoinsStore::ShardOp> ops;
    };
    std::array<GroupData, COMMIT_GROUPS> group_data;

    //! Phase 1 per group: 0 unclaimed, 1 in progress, 2 done.
    std::array<std::atomic<uint8_t>, COMMIT_GROUPS> check_state{};
    std::atomic<size_t> checks_done{0};
    std::atomic<uint32_t> first_conflict{NO_CONFLICT};
    std::atomic<bool> phase2_ready{false};
    std::atomic<uint32_t> valid_blocks{0};

    //! Phase 2 per group: 0 unclaimed, 1 in progress, 2 done.
    std::array<std::atomic<uint8_t>, COMMIT_GROUPS> apply_state{};
    std::atomic<size_t> applies_done{0};
    std::atomic<bool> finalizing{false};

    Mutex error_mutex;
    std::exception_ptr error GUARDED_BY(error_mutex);
    bool ready{false};
};

struct WindowSync {
    Mutex mutex;
    std::condition_variable cv;
    std::deque<StagedConnect*> slots GUARDED_BY(mutex);
    WindowOutputs outputs;
    //! At most one commit job runs at a time, started and waited on by the publishing thread.
    std::shared_ptr<CommitJob> active_job GUARDED_BY(mutex);
    bool draining GUARDED_BY(mutex){false};
    std::atomic<bool> job_open{false};
};

size_t ParsedBlockBytes(const CBlock& block, size_t serialized, size_t& inputs);
size_t WindowBlockBytes(const CBlock& block, int num_inputs, size_t block_bytes);

//! Resolve all non-coinbase inputs of `block` at `viewer_height` via WindowOutputs -> CoinsStore -> sorted BaseCoin().
void ResolveWindowInputs(const CoinsStore& store,
                         const WindowOutputs& outputs,
                         int viewer_height,
                         const CBlock& block,
                         std::vector<Coin>& coins,
                         std::vector<uint8_t>& from_base);

//! BIP30 check for `block` at `viewer_height` against WindowOutputs, CoinsStore, and backing DB.
bool WindowHaveAnyOutput(const CoinsStore& store,
                         const WindowOutputs& outputs,
                         int viewer_height,
                         const CBlock& block);

//! Build compact shard-grouped CommitItems from a validated block and its undo data.
void PrepareCommit(StagedConnect& s, const CBlock& block, const CBlockUndo& undo, std::span<const uint8_t> from_base = {});

//! Create a 2-phase CommitJob covering `front` and up to `max_blocks - 1` contiguous ready blocks above it.
std::shared_ptr<CommitJob> StartCommitJob(WindowSync& sync, StagedConnect& front, size_t max_blocks, int max_height)
    EXCLUSIVE_LOCKS_REQUIRED(sync.mutex);

//! Cooperatively execute Phase 1 (check) and Phase 2 (apply) of the active CommitJob.
//! Captures std::exception in job->error.
void RunCommitWork(WindowSync& sync) noexcept;

//! Wait for every in-flight block in `in_flight` to finish and clear its slot.
void DrainWindow(std::shared_ptr<WindowSync>& sync,
                 std::deque<std::unique_ptr<StagedConnect>>& in_flight);

} // namespace node

#endif // BITCOIN_NODE_CONNECT_WINDOW_H
