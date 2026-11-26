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
#include <util/byte_ledger.h>
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
#include <map>
#include <memory>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

struct BlockValidationContext;
struct ConnectBlockResult;

namespace node {

inline constexpr size_t COMMIT_GROUPS{64};
inline constexpr size_t MAX_COMMIT_BLOCKS{32};
inline constexpr size_t MAX_COMMIT_BLOCKS_LARGE{8};
inline constexpr size_t COMMIT_ITEMS_PER_HELPER{256};
inline constexpr size_t COMMIT_WORKERS_PER_HELPER{4};
inline constexpr size_t MAX_COMMIT_HELPERS{16};

inline constexpr size_t WINDOW_BYTES_SHARE{8};
inline constexpr size_t MIN_WINDOW_BYTES{32_MiB};
inline constexpr size_t MIN_PINNED_BYTES{64_MiB};
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

/**
 * Per-thread deferred CBlock destructor bins so blocks parsed by connect-pool workers are freed
 * on the thread that allocated them rather than contending on the publisher thread's allocator.
 */
class BlockBins
{
    struct Bin {
        Mutex mutex;
        std::vector<CBlock*> blocks GUARDED_BY(mutex);
        std::atomic<bool> any{false};
        void Empty() EXCLUSIVE_LOCKS_REQUIRED(!mutex)
        {
            if (!any.exchange(false, std::memory_order_acq_rel)) return;
            for (const CBlock* block : WITH_LOCK(mutex, return std::exchange(blocks, {}))) delete block;
        }
        ~Bin() { Empty(); }
    };

    static inline std::atomic<uint64_t> s_next_id{1};
    const uint64_t m_id{s_next_id.fetch_add(1, std::memory_order_relaxed)};
    Mutex m_mutex;
    std::map<std::thread::id, std::shared_ptr<Bin>> m_bins GUARDED_BY(m_mutex);

public:
    std::shared_ptr<CBlock> NewBlock() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex)
    {
        std::shared_ptr<Bin> bin;
        {
            LOCK(m_mutex);
            auto& mine{m_bins[std::this_thread::get_id()]};
            if (!mine) mine = std::make_shared<Bin>();
            bin = mine;
        }
        bin->Empty();
        return {new CBlock, [bin](CBlock* block) {
            WITH_LOCK(bin->mutex, bin->blocks.push_back(block));
            bin->any.store(true, std::memory_order_release);
        }};
    }

    void EmptyMine() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex)
    {
        static thread_local std::pair<uint64_t, std::shared_ptr<Bin>> mine{0, nullptr};
        if (mine.first != m_id) {
            LOCK(m_mutex);
            const auto it{m_bins.find(std::this_thread::get_id())};
            if (it == m_bins.end()) return;
            mine = {m_id, it->second};
        }
        mine.second->Empty();
    }

    void Empty() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex)
    {
        LOCK(m_mutex);
        for (const auto& [_, bin] : m_bins) bin->Empty();
    }
};

//! Tracks how many committed blocks are ahead of the published chain tip inside a publication hold.
struct CommitGate {
    std::atomic<int> ahead{0};
};

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
    uint32_t undo_size{0};
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

    //! Per group: 0 unclaimed, 1 in progress, 2 done.
    std::array<std::atomic<uint8_t>, COMMIT_GROUPS> group_state{};
    std::atomic<size_t> groups_done{0};
    std::atomic<size_t> next_clean{0};
    std::atomic<size_t> cleaned{0};
    std::atomic<uint32_t> first_conflict{NO_CONFLICT};

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
void PrepareCommit(StagedConnect& s, const CBlock& block, const CBlockUndo& undo);

//! Create a CommitJob covering `front` and up to `max_blocks - 1` contiguous ready blocks above it.
std::shared_ptr<CommitJob> StartCommitJob(WindowSync& sync, StagedConnect& front, size_t max_blocks, int max_height)
    EXCLUSIVE_LOCKS_REQUIRED(sync.mutex);

//! Claim and apply uncommitted shard groups of the active CommitJob. Captures std::runtime_error in job->error.
void RunCommitWork(WindowSync& sync) noexcept;

//! Roll back the first conflicting block (or block 0 on fatal error) and all blocks above it from CoinsStore.
[[nodiscard]] bool TakeBackCommit(CommitJob& job,
                                  uint32_t first_to_revert,
                                  const std::function<bool(const CBlockIndex&, CBlockUndo&)>& read_undo);

//! Wait for every in-flight block in `in_flight` to finish, release its view, and clear `window_account`.
void DrainWindow(std::shared_ptr<WindowSync>& sync,
                 std::deque<std::unique_ptr<StagedConnect>>& in_flight,
                 util::ByteAccount& window_account);

} // namespace node

#endif // BITCOIN_NODE_CONNECT_WINDOW_H
