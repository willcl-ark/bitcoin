// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_NODE_CONNECT_WINDOW_H
#define BITCOIN_NODE_CONNECT_WINDOW_H

#include <block_apply_view.h>
#include <chain.h>
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
#include <functional>
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
inline constexpr size_t MIN_WINDOW_BYTES{16_MiB};
inline constexpr size_t MAX_WINDOW_BYTES{256_MiB};
inline constexpr size_t MIN_PINNED_BYTES{16_MiB};
inline constexpr size_t MAX_PINNED_BYTES{256_MiB};
inline constexpr size_t MAX_UNDO_BACKLOG_BYTES{64_MiB};
inline constexpr size_t WINDOW_BYTES_PER_ITEM{512};
inline constexpr size_t VIEW_BYTES_PER_ENTRY{128};

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
    BlockApplyView* view{nullptr};
    const char* script_check_reason{nullptr};
    bool write_undo{true};

    std::shared_ptr<const CBlock> block;
    std::shared_ptr<BlockValidationContext> ctx;
    bool read_failed{false};
    bool ok{false};
    BlockValidationState state;
    std::shared_ptr<CBlockUndo> undo;
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
    std::vector<BlockApplyView::CommitItem> items;
    bool committed{false};
    bool commit_failed{false};
    bool in_job{false};
};

struct CommitJob {
    static constexpr uint32_t NO_CONFLICT{std::numeric_limits<uint32_t>::max()};

    size_t items{0};
    std::vector<StagedConnect*> blocks;
    std::vector<int> heights;
    uint64_t store_flush_gen{0};

    //! Per group: 0 unclaimed, 1 in progress, 2 done.
    std::array<std::atomic<uint8_t>, COMMIT_GROUPS> group_state{};
    std::atomic<size_t> groups_done{0};
    std::atomic<uint32_t> first_conflict{NO_CONFLICT};
    std::atomic<bool> finalizing{false};

    Mutex report_mutex;
    struct Overwritten {
        uint32_t b;
        uint32_t i;
        Coin coin;
    };
    std::vector<Overwritten> overwritten GUARDED_BY(report_mutex);
    std::vector<std::pair<uint32_t, COutPoint>> unread GUARDED_BY(report_mutex);
    std::exception_ptr error GUARDED_BY(report_mutex);
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

size_t UndoBytes(const CBlockUndo& undo);
size_t ParsedBlockBytes(const CBlock& block, size_t serialized, size_t& inputs);
size_t WindowBlockBytes(const CBlock& block, int num_inputs, size_t block_bytes);

//! Build compact shard-grouped CommitItems from a validated block and its undo data, and empty the view.
void PrepareCommit(StagedConnect& s, const CBlock& block, const CBlockUndo& undo, std::span<const uint8_t> from_base = {});

//! Create a CommitJob covering `front` and up to `max_blocks - 1` contiguous ready blocks above it.
std::shared_ptr<CommitJob> StartCommitJob(WindowSync& sync, StagedConnect& front, size_t max_blocks, int max_height)
    EXCLUSIVE_LOCKS_REQUIRED(sync.mutex);

//! Claim and apply uncommitted shard groups of the active CommitJob. Captures std::exception in job->error.
void RunCommitWork(WindowSync& sync) noexcept;

//! Roll back `first_to_revert` and all blocks above it in `job` from CoinsStore.
[[nodiscard]] bool TakeBackCommit(CommitJob& job,
                                  uint32_t first_to_revert,
                                  const std::function<bool(const CBlockIndex&, CBlockUndo&)>& read_undo);

//! Wait for every in-flight block in `in_flight` to finish and release its view.
void DrainWindow(std::shared_ptr<WindowSync>& sync,
                 std::deque<std::unique_ptr<StagedConnect>>& in_flight);

} // namespace node

#endif // BITCOIN_NODE_CONNECT_WINDOW_H
