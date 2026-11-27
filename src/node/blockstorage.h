// Copyright (c) 2011-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_NODE_BLOCKSTORAGE_H
#define BITCOIN_NODE_BLOCKSTORAGE_H

#include <attributes.h>
#include <chain.h>
#include <dbwrapper.h>
#include <flatfile.h>
#include <kernel/blockmanager_opts.h>
#include <kernel/chainparams.h>
#include <kernel/cs_main.h>
#include <kernel/messagestartchars.h>
#include <primitives/block.h>
#include <serialize.h>
#include <streams.h>
#include <sync.h>
#include <uint256.h>
#include <util/byte_units.h>
#include <util/expected.h>
#include <util/fs.h>
#include <util/hasher.h>
#include <util/obfuscation.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <iosfwd>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <shared_mutex>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

class BlockValidationState;
class CBlockUndo;
class Chainstate;
class ChainstateManager;
namespace Consensus {
struct Params;
}
namespace util {
class SignalInterrupt;
} // namespace util

namespace kernel {
class CBlockFileInfo
{
public:
    uint32_t nBlocks{};      //!< number of blocks stored in file
    uint64_t nSize{};        //!< number of used bytes of block file
    uint64_t nUndoSize{};    //!< number of used bytes in the undo file
    uint32_t nHeightFirst{}; //!< lowest height of block in file
    uint32_t nHeightLast{};  //!< highest height of block in file
    uint64_t nTimeFirst{};   //!< earliest time of block in file
    uint64_t nTimeLast{};    //!< latest time of block in file

    SERIALIZE_METHODS(CBlockFileInfo, obj)
    {
        READWRITE(VARINT(obj.nBlocks));
        READWRITE(VARINT(obj.nSize));
        READWRITE(VARINT(obj.nUndoSize));
        READWRITE(VARINT(obj.nHeightFirst));
        READWRITE(VARINT(obj.nHeightLast));
        READWRITE(VARINT(obj.nTimeFirst));
        READWRITE(VARINT(obj.nTimeLast));
    }

    CBlockFileInfo() = default;

    std::string ToString() const;

    /** update statistics (does not update nSize) */
    void AddBlock(unsigned int nHeightIn, uint64_t nTimeIn)
    {
        if (nBlocks == 0 || nHeightFirst > nHeightIn)
            nHeightFirst = nHeightIn;
        if (nBlocks == 0 || nTimeFirst > nTimeIn)
            nTimeFirst = nTimeIn;
        nBlocks++;
        if (nHeightIn > nHeightLast)
            nHeightLast = nHeightIn;
        if (nTimeIn > nTimeLast)
            nTimeLast = nTimeIn;
    }
};

/** Access to the block database (blocks/index/) */
class BlockTreeDB : public CDBWrapper
{
public:
    using CDBWrapper::CDBWrapper;
    void WriteBatchSync(const std::vector<std::pair<int, const CBlockFileInfo*>>& fileInfo, int nLastFile, const std::vector<const CBlockIndex*>& blockinfo);
    bool ReadBlockFileInfo(int nFile, CBlockFileInfo& info);
    bool ReadLastBlockFile(int& nFile);
    void WriteReindexing(bool fReindexing);
    void ReadReindexing(bool& fReindexing);
    void WriteFlag(const std::string& name, bool fValue);
    bool ReadFlag(const std::string& name, bool& fValue);
    bool LoadBlockIndexGuts(const Consensus::Params& consensusParams, std::function<CBlockIndex*(const uint256&)> insertBlockIndex, const util::SignalInterrupt& interrupt)
        EXCLUSIVE_LOCKS_REQUIRED(::cs_main);
};
} // namespace kernel

namespace node {
using kernel::CBlockFileInfo;
using kernel::BlockTreeDB;

/** The pre-allocation chunk size for blk?????.dat files (since 0.8) */
inline constexpr uint64_t BLOCKFILE_CHUNK_SIZE{16_MiB};
/** The pre-allocation chunk size for rev?????.dat files (since 0.8) */
inline constexpr uint64_t UNDOFILE_CHUNK_SIZE{1_MiB};
/** The maximum size of a blk?????.dat file (since 0.8) */
inline constexpr uint64_t MAX_BLOCKFILE_SIZE{128_MiB};

/** Size of header written by WriteBlock before a serialized CBlock (8 bytes) */
inline constexpr uint64_t STORAGE_HEADER_BYTES{std::tuple_size_v<MessageStartChars> + sizeof(uint32_t)};

/** Total overhead when writing undo data: header (8 bytes) plus checksum (32 bytes) */
inline constexpr uint64_t UNDO_DATA_DISK_OVERHEAD{STORAGE_HEADER_BYTES + uint256::size()};

// Because validation code takes pointers to the map's CBlockIndex objects, if
// we ever switch to another associative container, we need to either use a
// container that has stable addressing (true of all std associative
// containers), or make the key a `std::unique_ptr<CBlockIndex>`
using BlockMap = std::unordered_map<uint256, CBlockIndex, BlockHasher>;

struct CBlockIndexWorkComparator {
    bool operator()(const CBlockIndex* pa, const CBlockIndex* pb) const;
    using is_transparent = void;
};

struct CBlockIndexHeightOnlyComparator {
    /* Only compares the height of two block indices, doesn't try to tie-break */
    bool operator()(const CBlockIndex* pa, const CBlockIndex* pb) const;
};

struct PruneLockInfo {
    /// Height of earliest block that should be kept and not pruned
    int height_first{std::numeric_limits<int>::max()};
};

enum BlockfileType {
    // Values used as array indexes - do not change carelessly.
    NORMAL = 0,
    ASSUMED = 1,
    NUM_TYPES = 2,
};

std::ostream& operator<<(std::ostream& os, const BlockfileType& type);

struct BlockfileCursor {
    // The latest blockfile number.
    int file_num{0};

    // Track the height of the highest block in file_num whose undo
    // data has been written. Block data is written to block files in download
    // order, but is written to undo files in validation order, which is
    // usually in order by height. To avoid wasting disk space, undo files will
    // be trimmed whenever the corresponding block file is finalized and
    // the height of the highest block written to the block file equals the
    // height of the highest block written to the undo file. This is a
    // heuristic and can sometimes preemptively trim undo files that will write
    // more data later, and sometimes fail to trim undo files that can't have
    // more data written later.
    int undo_height{0};
};

std::ostream& operator<<(std::ostream& os, const BlockfileCursor& cursor);

enum class ReadRawError {
    IO,
    BadPartRange,
};

/** Pre-serialized undo record: [4B magic][4B payload_size][CBlockUndo][32B checksum]. */
struct SerializedBlockUndo {
    std::vector<unsigned char> data;
};

/**
 * Maintains a tree of blocks (stored in `m_block_index`) which is consulted
 * to determine where the most-work tip is.
 *
 * This data is used mostly in `Chainstate` - information about, e.g.,
 * candidate tips is not maintained here.
 */
class BlockManager
{
    friend Chainstate;
    friend ChainstateManager;

private:
    const CChainParams& GetParams() const { return m_opts.chainparams; }
    const Consensus::Params& GetConsensus() const { return m_opts.chainparams.GetConsensus(); }
    /**
     * Load the blocktree off disk and into memory. Populate certain metadata
     * per index entry (nStatus, nChainWork, nTimeMax, etc.) as well as peripheral
     * collections like m_dirty_blockindex.
     */
    bool LoadBlockIndex(const std::optional<uint256>& snapshot_blockhash)
        EXCLUSIVE_LOCKS_REQUIRED(cs_main);

    /** Return false if block file or undo file flushing fails. */
    [[nodiscard]] bool FlushBlockFile(int blockfile_num, bool fFinalize, bool finalize_undo) EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

    /** Return false if undo file flushing fails. */
    [[nodiscard]] bool FlushUndoFile(int block_file, bool finalize = false);

    /**
     * Helper function performing various preparations before a block can be saved to disk:
     * Returns the correct position for the block to be saved, which may be in the current or a new
     * block file depending on nAddSize. May trim the previous blockfile if full, updates
     * blockfile info, and checks if there is enough disk space to save the block.
     *
     * The nAddSize argument passed to this function should include not just the size of the serialized CBlock, but also the size of
     * separator fields (STORAGE_HEADER_BYTES).
     */
    [[nodiscard]] FlatFilePos FindNextBlockPos(uint64_t nAddSize, unsigned int nHeight, uint64_t nTime, bool update_block_stats = true)
        EXCLUSIVE_LOCKS_REQUIRED(::cs_main, !m_blk_handle_mutex, !m_undo_queue_mutex);
    [[nodiscard]] bool FlushChainstateBlockFile(int tip_height) EXCLUSIVE_LOCKS_REQUIRED(::cs_main);
    [[nodiscard]] bool FlushUnsyncedFiles(int tip_height)
        EXCLUSIVE_LOCKS_REQUIRED(::cs_main, !m_blk_handle_mutex);
    [[nodiscard]] bool FindUndoPos(BlockValidationState& state, int nFile, FlatFilePos& pos, uint64_t nAddSize)
        EXCLUSIVE_LOCKS_REQUIRED(::cs_main, !m_undo_queue_mutex);

    AutoFile OpenUndoFile(const FlatFilePos& pos, bool fReadOnly = false) const;

    /** Persistent blk*.dat write handle and in-flight off-cs_main write tracker.
     *  Lock order: cs_main -> m_blk_handle_mutex (never acquires cs_main). */
    mutable Mutex m_blk_handle_mutex;
    mutable std::condition_variable m_blk_writes_cv;
    mutable FILE* m_blk_handle GUARDED_BY(m_blk_handle_mutex){nullptr};
    mutable int m_blk_handle_file GUARDED_BY(m_blk_handle_mutex){-1};
    mutable uint64_t m_blk_handle_pos GUARDED_BY(m_blk_handle_mutex){0};
    mutable size_t m_inflight_blk_writes GUARDED_BY(m_blk_handle_mutex){0};

    void FinishInflightBlockWrite() const noexcept EXCLUSIVE_LOCKS_REQUIRED(!m_blk_handle_mutex);

    /** Serializes concurrent off-cs_main disk stores of the same CBlockIndex to avoid duplicate writes.
     *  Leaf mutex: never acquires cs_main or any other BlockManager mutex while held. */
    mutable Mutex m_inflight_store_mutex;
    mutable std::condition_variable m_inflight_store_cv;
    mutable std::unordered_set<const CBlockIndex*> m_inflight_store_blocks GUARDED_BY(m_inflight_store_mutex);

    void ReleaseInflightBlockStore(const CBlockIndex* pindex) const noexcept EXCLUSIVE_LOCKS_REQUIRED(!m_inflight_store_mutex);

    /** Platform-agnostic concurrent read handle cache for parallel blk*.dat reads during IBD.
     *  Never acquires cs_main; starts empty with zero open descriptors or threads. */
#ifndef WIN32
    struct ReadFd {
        int fd{-1};
        bool drop_cache_on_close{false};
        explicit ReadFd(int f) noexcept : fd{f} {}
        ~ReadFd();
        ReadFd(const ReadFd&) = delete;
        ReadFd& operator=(const ReadFd&) = delete;
    };

    struct BlockReadFdCache {
        static constexpr size_t MAX_CACHED_FDS = 16;
        struct CacheEntry {
            int file_num{-1};
            uint64_t insert_seq{0};
            std::shared_ptr<ReadFd> fd;
        };
        mutable std::shared_mutex mutex;
        std::array<CacheEntry, MAX_CACHED_FDS> entries{};
        uint64_t next_seq{0};

        std::shared_ptr<ReadFd> GetOrOpen(int file_num, const fs::path& path);
        void Clear(bool drop_cache = false);
    };
    mutable BlockReadFdCache m_read_fd_cache;
#else
    mutable Mutex m_concurrent_read_mutex;
    mutable std::condition_variable m_concurrent_read_cv;
    mutable size_t m_active_concurrent_reads GUARDED_BY(m_concurrent_read_mutex){0};
    static constexpr size_t MAX_CONCURRENT_WINDOWS_READS = 8;
#endif

    /** Track modified blk*.dat and rev*.dat files not yet fsynced in FlushStateToDisk. */
    std::set<int> m_unsynced_block_files GUARDED_BY(::cs_main);
    mutable std::set<int> m_unsynced_undo_files GUARDED_BY(::cs_main);

    /** Pin block files that have reserved-but-not-yet-published off-cs_main writes so pruning
     *  never deletes a file before its block is published or aborted. */
    std::unordered_map<int, size_t> m_unpublished_block_files GUARDED_BY(::cs_main);

    /** Permanently latched on any block/undo write or sync I/O failure. */
    mutable std::atomic<bool> m_storage_io_error{false};

    /** Optional fault-injection hook invoked before writing undo data (both inline and async paths). */
    std::atomic<bool> m_has_undo_fault_hook{false};
    mutable Mutex m_fault_hook_mutex;
    std::function<bool(const uint256& block_hash, int height)> m_undo_write_fault_hook GUARDED_BY(m_fault_hook_mutex);
    bool ShouldInjectUndoWriteFault(const uint256& block_hash, int height) const
        EXCLUSIVE_LOCKS_REQUIRED(!m_fault_hook_mutex);

    /** Lazy, stoppable background writer for async IBD undo writes, undo trims, and prune unlinks.
     *  Lock order: cs_main -> m_undo_lifecycle_mutex -> m_undo_queue_mutex.
     *  The worker thread never acquires cs_main. */
    struct UndoJob {
        enum class Type : uint8_t {
            WRITE_UNDO,
            TRIM_UNDO,
            UNLINK_FILES,
            CLOSE_HANDLE,
        };
        Type type{Type::WRITE_UNDO};
        CBlockIndex* pindex{nullptr};
        uint256 block_hash;
        int block_height{0};
        FlatFilePos pos;
        SerializedBlockUndo payload;
        size_t budgeted_bytes{0};
        FlatFilePos trim_pos;
        std::set<int> files_to_unlink;
    };

    struct CompletedUndo {
        CBlockIndex* pindex{nullptr};
        int file_num{-1};
        uint64_t undo_pos{0};
    };

    mutable Mutex m_undo_lifecycle_mutex;
    mutable Mutex m_undo_queue_mutex;
    mutable std::condition_variable m_undo_cv;
    mutable std::condition_variable m_undo_settled_cv;
    mutable std::deque<UndoJob> m_undo_queue GUARDED_BY(m_undo_queue_mutex);
    mutable std::vector<CompletedUndo> m_completed_undos GUARDED_BY(m_undo_queue_mutex);
    mutable bool m_undo_worker_busy GUARDED_BY(m_undo_queue_mutex){false};
    mutable bool m_undo_stop GUARDED_BY(m_undo_queue_mutex){false};
    std::thread m_undo_thread GUARDED_BY(m_undo_lifecycle_mutex);
    mutable std::atomic<bool> m_undo_thread_running{false};
    std::atomic<bool> m_undo_stopping{false};
    mutable std::atomic<size_t> m_pending_undo_work{0};
    std::atomic<size_t>* m_pending_undo_budget_bytes{nullptr};

    [[nodiscard]] bool EnsureAsyncUndoWriterStarted()
        EXCLUSIVE_LOCKS_REQUIRED(!m_undo_lifecycle_mutex, !m_undo_queue_mutex);
    void StopAsyncUndoWriterImpl()
        EXCLUSIVE_LOCKS_REQUIRED(!m_undo_lifecycle_mutex, !m_undo_queue_mutex, !m_blk_handle_mutex);
    void AsyncUndoWorkerLoop()
        EXCLUSIVE_LOCKS_REQUIRED(!m_undo_queue_mutex, !m_fault_hook_mutex);
    void UnlinkFilesNow(const std::set<int>& setFilesToPrune) const;
    bool WriteSerializedBlockUndoInline(SerializedBlockUndo& serialized, BlockValidationState& state, CBlockIndex& block)
        EXCLUSIVE_LOCKS_REQUIRED(::cs_main, !m_undo_queue_mutex, !m_fault_hook_mutex);

    /* Calculate the block/rev files to delete based on height specified by user with RPC command pruneblockchain */
    void FindFilesToPruneManual(
        std::set<int>& setFilesToPrune,
        int nManualPruneHeight,
        const Chainstate& chain);

    /**
     * Prune block and undo files (blk???.dat and rev???.dat) so that the disk space used is less than a user-defined target.
     * The user sets the target (in MB) on the command line or in config file.  This will be run on startup and whenever new
     * space is allocated in a block or undo file, staying below the target. Changing back to unpruned requires a reindex
     * (which in this case means the blockchain must be re-downloaded.)
     *
     * Pruning functions are called from FlushStateToDisk when the m_check_for_pruning flag has been set.
     * Block and undo files are deleted in lock-step (when blk00003.dat is deleted, so is rev00003.dat.)
     * Pruning cannot take place until the longest chain is at least a certain length (CChainParams::nPruneAfterHeight).
     * Pruning will never delete a block within a defined distance (currently 288) from the active chain's tip.
     * The block index is updated by unsetting HAVE_DATA and HAVE_UNDO for any blocks that were stored in the deleted files.
     * A db flag records the fact that at least some block files have been pruned.
     *
     * @param[out]   setFilesToPrune   The set of file indices that can be unlinked will be returned
     * @param        last_prune        The last height we're able to prune, according to the prune locks
     */
    void FindFilesToPrune(
        std::set<int>& setFilesToPrune,
        int last_prune,
        const Chainstate& chain,
        ChainstateManager& chainman);

    //! Since assumedvalid chainstates may be syncing a range of the chain that is very
    //! far away from the normal/background validation process, we should segment blockfiles
    //! for assumed chainstates. Otherwise, we might have wildly different height ranges
    //! mixed into the same block files, which would impair our ability to prune
    //! effectively.
    //!
    //! This data structure maintains separate blockfile number cursors for each
    //! BlockfileType. The ASSUMED state is initialized, when necessary, in FindNextBlockPos().
    //!
    //! The first element is the NORMAL cursor, second is ASSUMED.
    std::array<std::optional<BlockfileCursor>, BlockfileType::NUM_TYPES>
        m_blockfile_cursors GUARDED_BY(::cs_main) = {
            BlockfileCursor{},
            std::nullopt,
    };
    int MaxBlockfileNum() const EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
    {
        AssertLockHeld(::cs_main);
        static const BlockfileCursor empty_cursor;
        const auto& normal = m_blockfile_cursors[BlockfileType::NORMAL].value_or(empty_cursor);
        const auto& assumed = m_blockfile_cursors[BlockfileType::ASSUMED].value_or(empty_cursor);
        return std::max(normal.file_num, assumed.file_num);
    }

    /** Global flag to indicate we should check to see if there are
     *  block/undo files that should be deleted.  Set on startup
     *  or if we allocate more file space when we're in prune mode
     */
    bool m_check_for_pruning = false;

    const bool m_prune_mode;

    const Obfuscation m_obfuscation;

    /**
     * Map from external index name to oldest block that must not be pruned.
     *
     * @note Internally, only blocks at height (height_first - PRUNE_LOCK_BUFFER - 1) and
     * below will be pruned, but callers should avoid assuming any particular buffer size.
     */
    std::unordered_map<std::string, PruneLockInfo> m_prune_locks GUARDED_BY(::cs_main);

    BlockfileType BlockfileTypeForHeight(int height);

    const kernel::BlockManagerOpts m_opts;

    const FlatFileSeq m_block_file_seq;
    const FlatFileSeq m_undo_file_seq;

protected:
    std::vector<CBlockFileInfo> m_blockfile_info;

    /** Dirty block index entries. */
    mutable std::set<CBlockIndex*> m_dirty_blockindex;

    /** Dirty block file entries. */
    std::set<int> m_dirty_fileinfo;

public:
    using Options = kernel::BlockManagerOpts;
    using ReadRawBlockResult = util::Expected<std::vector<std::byte>, ReadRawError>;

    /** RAII reservation keeping in-flight off-cs_main blk*.dat writes accounted for. */
    class BlockWriteReservation {
        const BlockManager* m_blockman{nullptr};
    public:
        FlatFilePos pos;
        BlockWriteReservation() = default;
        BlockWriteReservation(const BlockManager* bm, FlatFilePos p) : m_blockman{bm}, pos{p} {}
        BlockWriteReservation(const BlockWriteReservation&) = delete;
        BlockWriteReservation& operator=(const BlockWriteReservation&) = delete;
        BlockWriteReservation(BlockWriteReservation&& other) noexcept
            : m_blockman{std::exchange(other.m_blockman, nullptr)}, pos{other.pos} {}
        BlockWriteReservation& operator=(BlockWriteReservation&& other) noexcept
        {
            if (this != &other) {
                Release();
                m_blockman = std::exchange(other.m_blockman, nullptr);
                pos = other.pos;
            }
            return *this;
        }
        ~BlockWriteReservation() { Release(); }
        void Release() noexcept
        {
            if (m_blockman) {
                m_blockman->FinishInflightBlockWrite();
                m_blockman = nullptr;
            }
        }
    };

    /** RAII guard serializing concurrent off-cs_main disk writes for the same CBlockIndex. */
    class InflightBlockStoreGuard {
        const BlockManager* m_blockman{nullptr};
        const CBlockIndex* m_pindex{nullptr};
    public:
        InflightBlockStoreGuard() = default;
        InflightBlockStoreGuard(const BlockManager* bm, const CBlockIndex* pindex) noexcept
            : m_blockman{bm}, m_pindex{pindex} {}
        InflightBlockStoreGuard(const InflightBlockStoreGuard&) = delete;
        InflightBlockStoreGuard& operator=(const InflightBlockStoreGuard&) = delete;
        InflightBlockStoreGuard(InflightBlockStoreGuard&& other) noexcept
            : m_blockman{std::exchange(other.m_blockman, nullptr)},
              m_pindex{std::exchange(other.m_pindex, nullptr)} {}
        InflightBlockStoreGuard& operator=(InflightBlockStoreGuard&& other) noexcept
        {
            if (this != &other) {
                Release();
                m_blockman = std::exchange(other.m_blockman, nullptr);
                m_pindex = std::exchange(other.m_pindex, nullptr);
            }
            return *this;
        }
        ~InflightBlockStoreGuard() { Release(); }
        void Release() noexcept
        {
            if (m_blockman && m_pindex) {
                m_blockman->ReleaseInflightBlockStore(m_pindex);
                m_blockman = nullptr;
                m_pindex = nullptr;
            }
        }
    };

    /** Pre-reserved undo write slot for atomic batch IBD commits. */
    struct PreparedUndoWrite {
        CBlockIndex* pindex{nullptr};
        FlatFilePos pos;
        std::optional<FlatFilePos> trim_after;
        BlockfileType file_type{BlockfileType::NORMAL};
        int prev_cursor_undo_height{0};
        bool updated_cursor{false};
        uint64_t prev_undo_size{0};
        bool needs_write{false};
    };

    explicit BlockManager(const util::SignalInterrupt& interrupt, Options opts);
    ~BlockManager();

    const util::SignalInterrupt& m_interrupt;
    std::atomic<bool> m_importing{false};

    /**
     * Whether all blockfiles have been added to the block tree database.
     * Normally true, but set to false when a reindex is requested and the
     * database is wiped. The value is persisted in the database across restarts
     * and will be false until reindexing completes.
     */
    std::atomic_bool m_blockfiles_indexed{true};

    BlockMap m_block_index GUARDED_BY(cs_main);

    /**
     * The height of the base block of an assumeutxo snapshot, if one is in use.
     *
     * This controls how blockfiles are segmented by chainstate type to avoid
     * comingling different height regions of the chain when an assumedvalid chainstate
     * is in use. If heights are drastically different in the same blockfile, pruning
     * suffers.
     *
     * This is set during ActivateSnapshot() or upon LoadBlockIndex() if a snapshot
     * had been previously loaded. After the snapshot is validated, this is unset to
     * restore normal LoadBlockIndex behavior.
     */
    std::optional<int> m_snapshot_height;

    std::vector<CBlockIndex*> GetAllBlockIndices() EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

    /**
     * All pairs A->B, where A (or one of its ancestors) misses transactions, but B has transactions.
     */
    std::multimap<CBlockIndex*, CBlockIndex*> m_blocks_unlinked;
    void AddUnlinkedBlock(CBlockIndex* block) EXCLUSIVE_LOCKS_REQUIRED(cs_main);

    std::unique_ptr<BlockTreeDB> m_block_tree_db GUARDED_BY(::cs_main);

    void WriteBlockIndexDB() EXCLUSIVE_LOCKS_REQUIRED(::cs_main);
    bool LoadBlockIndexDB(const std::optional<uint256>& snapshot_blockhash)
        EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

    /**
     * Remove any pruned block & undo files that are still on disk.
     * This could happen on some systems if the file was still being read while unlinked,
     * or if we crash before unlinking.
     */
    void ScanAndUnlinkAlreadyPrunedFiles()
        EXCLUSIVE_LOCKS_REQUIRED(::cs_main, !m_blk_handle_mutex, !m_undo_queue_mutex);

    CBlockIndex* AddToBlockIndex(const CBlockHeader& block, CBlockIndex*& best_header) EXCLUSIVE_LOCKS_REQUIRED(cs_main);
    /** Create a new block index entry for a given block hash */
    CBlockIndex* InsertBlockIndex(const uint256& hash) EXCLUSIVE_LOCKS_REQUIRED(cs_main);

    //! Mark one block file as pruned (modify associated database entries)
    void PruneOneBlockFile(int fileNumber) EXCLUSIVE_LOCKS_REQUIRED(cs_main);

    CBlockIndex* LookupBlockIndex(const uint256& hash) EXCLUSIVE_LOCKS_REQUIRED(cs_main);
    const CBlockIndex* LookupBlockIndex(const uint256& hash) const EXCLUSIVE_LOCKS_REQUIRED(cs_main);

    /** Get block file info entry for one block file */
    CBlockFileInfo* GetBlockFileInfo(size_t n) EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

    bool WriteBlockUndo(const CBlockUndo& blockundo, BlockValidationState& state, CBlockIndex& block)
        EXCLUSIVE_LOCKS_REQUIRED(::cs_main, !m_undo_queue_mutex, !m_fault_hook_mutex);

    /** Single-pass undo serialization helper shared by inline tip writes and async IBD writes. */
    SerializedBlockUndo SerializeBlockUndoPayload(const CBlockUndo& blockundo, const uint256& prev_block_hash) const;

    /** Enqueue a pre-serialized undo record for lazy background writing during IBD. */
    bool EnqueueBlockUndoWrite(SerializedBlockUndo&& serialized, BlockValidationState& state, CBlockIndex& block)
        EXCLUSIVE_LOCKS_REQUIRED(::cs_main, !m_undo_lifecycle_mutex, !m_undo_queue_mutex);

    /** Pre-reserve undo file positions and ensure async writer is ready for an IBD commit batch. */
    [[nodiscard]] bool PrepareBatchUndoWrites(
        std::span<CBlockIndex* const> blocks,
        std::span<const size_t> serialized_sizes,
        std::vector<PreparedUndoWrite>& prepared,
        BlockValidationState& state)
        EXCLUSIVE_LOCKS_REQUIRED(::cs_main, !m_undo_lifecycle_mutex, !m_undo_queue_mutex);

    /** Roll back pre-reserved undo file positions from `from_index` to the end of `prepared`. */
    void RollbackPreparedBatchUndoWrites(std::span<const PreparedUndoWrite> prepared, size_t from_index)
        EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

    /** Enqueue a pre-reserved undo write for background persistence during an IBD batch commit. */
    void EnqueuePreparedUndoWrite(const PreparedUndoWrite& prep, SerializedBlockUndo&& serialized)
        EXCLUSIVE_LOCKS_REQUIRED(::cs_main, !m_undo_lifecycle_mutex, !m_undo_queue_mutex);

    /** Apply any completed async undo metadata to CBlockIndex entries under cs_main. */
    void ApplyCompletedUndos() const EXCLUSIVE_LOCKS_REQUIRED(::cs_main, !m_undo_queue_mutex);

    /** Drain all pending async undo writes, close any open rev*.dat handle, and apply metadata.
     *  Returns false if a storage I/O error is latched. */
    [[nodiscard]] bool SettlePendingUndoWrites() const EXCLUSIVE_LOCKS_REQUIRED(::cs_main, !m_undo_queue_mutex);

    /** Stop and join the async undo writer thread and close persistent file handles (off cs_main). */
    void StopAsyncUndoWriter()
        LOCKS_EXCLUDED(::cs_main)
        EXCLUSIVE_LOCKS_REQUIRED(!m_undo_lifecycle_mutex, !m_undo_queue_mutex, !m_blk_handle_mutex);

    /** Fast lock-free check whether any async undo jobs or unapplied completions are pending. */
    bool HasPendingUndoWrites() const noexcept { return m_pending_undo_work.load(std::memory_order_acquire) > 0; }

    /** Return whether the lazy async undo writer thread is currently active. */
    bool IsAsyncUndoWriterRunning() const noexcept { return m_undo_thread_running.load(std::memory_order_acquire); }

    /** Attach external IBD memory budget counter for pending serialized undo bytes. */
    void SetPendingUndoBudgetCounter(std::atomic<size_t>* counter) noexcept { m_pending_undo_budget_bytes = counter; }

    /** Latch or query permanent storage I/O failure state. */
    void LatchStorageIOError() const noexcept { m_storage_io_error.store(true, std::memory_order_release); }
    bool HasStorageIOError() const noexcept { return m_storage_io_error.load(std::memory_order_acquire); }

    /** Test-only fault-injection hook invoked before writing undo data on both inline and async paths. */
    void SetUndoWriteFaultHookForTest(std::function<bool(const uint256& block_hash, int height)> hook)
        EXCLUSIVE_LOCKS_REQUIRED(!m_fault_hook_mutex);

    /** Serialize concurrent off-cs_main disk stores of the same CBlockIndex. */
    [[nodiscard]] InflightBlockStoreGuard AcquireInflightBlockStore(const CBlockIndex* pindex) const
        LOCKS_EXCLUDED(::cs_main) EXCLUSIVE_LOCKS_REQUIRED(!m_inflight_store_mutex);

    /** Single-pass serialization of a block plus its 8-byte disk storage header (safe off cs_main). */
    std::vector<unsigned char> SerializeBlockForDisk(const CBlock& block) const;

    /** Reserve space in blk*.dat under cs_main and track an in-flight off-cs_main write. */
    BlockWriteReservation ReserveBlockPos(uint64_t total_size, unsigned int nHeight, uint64_t nTime)
        EXCLUSIVE_LOCKS_REQUIRED(::cs_main, !m_blk_handle_mutex, !m_undo_queue_mutex);

    /** Publish a previously reserved block write into CBlockFileInfo and unpin the block file. */
    void PublishReservedBlockPos(int nFile, unsigned int nHeight, uint64_t nTime)
        EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

    /** Release the pruning pin for a previously reserved block write that was aborted before publication. */
    void AbortReservedBlockPos(int nFile) EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

    /** Write a pre-serialized block payload at `pos` off cs_main using a persistent file handle. */
    [[nodiscard]] bool WriteRawBlockToDisk(const FlatFilePos& pos, std::vector<unsigned char>& raw_data, bool keep_handle_open) const
        EXCLUSIVE_LOCKS_REQUIRED(!m_blk_handle_mutex);

    /** Wait for in-flight block writes and close any open persistent blk*.dat write handle. */
    bool CloseBlockWriteHandle() const EXCLUSIVE_LOCKS_REQUIRED(!m_blk_handle_mutex);

    /** Store block on disk and update block file statistics.
     *
     * @param[in]  block        the block to be stored
     * @param[in]  nHeight      the height of the block
     *
     * @returns in case of success, the position to which the block was written to
     *          in case of an error, an empty FlatFilePos
     */
    FlatFilePos WriteBlock(const CBlock& block, int nHeight)
        EXCLUSIVE_LOCKS_REQUIRED(::cs_main, !m_blk_handle_mutex, !m_undo_queue_mutex);

    /** Update blockfile info while processing a block during reindex. The block must be available on disk.
     *
     * @param[in]  block        the block being processed
     * @param[in]  nHeight      the height of the block
     * @param[in]  pos          the position of the serialized CBlock on disk
     */
    void UpdateBlockInfo(const CBlock& block, unsigned int nHeight, const FlatFilePos& pos) EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

    /** Whether running in -prune mode. */
    [[nodiscard]] bool IsPruneMode() const { return m_prune_mode; }

    /** Attempt to stay below this number of bytes of block files. */
    [[nodiscard]] uint64_t GetPruneTarget() const { return m_opts.prune_target; }
    static constexpr auto PRUNE_TARGET_MANUAL{std::numeric_limits<uint64_t>::max()};

    [[nodiscard]] bool LoadingBlocks() const { return m_importing || !m_blockfiles_indexed; }

    /** Calculate the amount of disk space the block & undo files currently use */
    uint64_t CalculateCurrentUsage() EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

    //! Check if all blocks in the [upper_block, lower_block] range have data available as
    //! defined by the status mask.
    //! The caller is responsible for ensuring that lower_block is an ancestor of upper_block
    //! (part of the same chain).
    bool CheckBlockDataAvailability(const CBlockIndex& upper_block, const CBlockIndex& lower_block, BlockStatus block_status = BLOCK_HAVE_DATA)
        EXCLUSIVE_LOCKS_REQUIRED(::cs_main, !m_undo_queue_mutex);

    /**
     * @brief Returns the earliest block with specified `status_mask` flags set after
     * the latest block _not_ having those flags.
     *
     * This function starts from `upper_block`, which must have all
     * `status_mask` flags set, and iterates backwards through its ancestors. It
     * continues as long as each block has all `status_mask` flags set, until
     * reaching the oldest ancestor or `lower_block`.
     *
     * @pre `upper_block` must have all `status_mask` flags set.
     * @pre `lower_block` must be null or an ancestor of `upper_block`
     *
     * @param upper_block The starting block for the search, which must have all
     *                    `status_mask` flags set.
     * @param status_mask Bitmask specifying required status flags.
     * @param lower_block The earliest possible block to return. If null, the
     *                    search can extend to the genesis block.
     *
     * @return A reference to the earliest block between `upper_block`
     *         and `lower_block`, inclusive, such that every block between the
     *         returned block and `upper_block` has `status_mask` flags set.
     */
    const CBlockIndex& GetFirstBlock(
        const CBlockIndex& upper_block LIFETIMEBOUND,
        uint32_t status_mask,
        const CBlockIndex* lower_block LIFETIMEBOUND = nullptr
    ) const EXCLUSIVE_LOCKS_REQUIRED(::cs_main, !m_undo_queue_mutex);

    /** True if any block files have ever been pruned. */
    bool m_have_pruned = false;

    //! Check whether the block associated with this index entry is pruned or not.
    bool IsBlockPruned(const CBlockIndex& block) const EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

    //! Create or update a prune lock identified by its name
    void UpdatePruneLock(const std::string& name, const PruneLockInfo& lock_info) EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

    //! Delete a prune lock identified by its name. Returns true if the lock existed.
    bool DeletePruneLock(const std::string& name) EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

    /** Open a block file (blk?????.dat) */
    AutoFile OpenBlockFile(const FlatFilePos& pos, bool fReadOnly) const;

    /** Translation to a filesystem path */
    fs::path GetBlockPosFilename(const FlatFilePos& pos) const;

    /**
     *  Actually unlink the specified files
     */
    void UnlinkPrunedFiles(const std::set<int>& setFilesToPrune) const
        EXCLUSIVE_LOCKS_REQUIRED(!m_blk_handle_mutex, !m_undo_queue_mutex);

    /** Functions for disk access for blocks */
    bool DeserializeAndCheckBlock(CBlock& block, std::span<const std::byte> block_data, const FlatFilePos& pos, const std::optional<uint256>& expected_hash) const;
    bool ReadBlock(CBlock& block, const FlatFilePos& pos, const std::optional<uint256>& expected_hash) const;
    bool ReadBlock(CBlock& block, const CBlockIndex& index) const;
    /** Concurrent lock-free block read/decode safe for parallel IBD workers off cs_main. */
    bool ReadBlockConcurrent(CBlock& block, const FlatFilePos& pos, const std::optional<uint256>& expected_hash) const
#ifdef WIN32
        EXCLUSIVE_LOCKS_REQUIRED(!m_concurrent_read_mutex)
#endif
        ;
    /** Best-effort OS readahead hint for a contiguous byte range in a blk*.dat file (safe off cs_main). */
    void AdviseBlockRange(const FlatFilePos& pos, uint64_t offset, uint64_t length) const;
    /** Release any cached concurrent blk*.dat read handles. */
    void CloseConcurrentReadHandles(bool drop_cache = false) const;
    ReadRawBlockResult ReadRawBlock(const FlatFilePos& pos, std::optional<std::pair<size_t, size_t>> block_part = std::nullopt) const;

    bool ReadBlockUndo(CBlockUndo& blockundo, const CBlockIndex& index) const
        EXCLUSIVE_LOCKS_REQUIRED(!m_undo_queue_mutex);

    void CleanupBlockRevFiles() const;
};

// Calls ActivateBestChain() even if no blocks are imported.
void ImportBlocks(ChainstateManager& chainman, std::span<const fs::path> import_paths);
} // namespace node

#endif // BITCOIN_NODE_BLOCKSTORAGE_H
