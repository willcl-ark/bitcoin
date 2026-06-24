// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <kernel/blocktreestorage.h>

#include <chain.h>
#include <crc32c/include/crc32c/crc32c.h>
#include <kernel/cs_main.h>
#include <logging.h>
#include <node/blockstorage.h>
#include <pow.h>
#include <serialize.h>
#include <span.h>
#include <streams.h>
#include <sync.h>
#include <tinyformat.h>
#include <uint256.h>
#include <util/check.h>
#include <util/fs.h>
#include <util/fs_helpers.h>
#include <util/signalinterrupt.h>
#include <util/time.h>

#include <array>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <ios>
#include <limits>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <type_traits>
#include <utility>

namespace kernel {

using Checksum = uint32_t;
using FilePosition = int64_t;

static constexpr const char* STORE_ACCESS_LOCK_NAME{".lock"};
static constexpr const char* WRITER_LOCK_NAME{".writer-lock"};
static constexpr uint32_t FLAG_FILE_MAGIC{0x72881d53}; // sha256sum("BLOCK_TREE_FLAG_FILE_MAGIC")
static constexpr uint32_t FLAG_FILE_VERSION{1};
static constexpr int64_t FLAG_FILE_DATA_START_POSITION{8}; // after magic (4bytes), version (4bytes)
static constexpr size_t FLAG_FILE_RECORD_SIZE{sizeof(uint8_t) + sizeof(Checksum)};

static GlobalMutex g_block_tree_store_locks_mutex;
static std::set<std::string> g_block_tree_store_locks GUARDED_BY(g_block_tree_store_locks_mutex);

static fs::path NormalizeDirectoryPath(const fs::path& dir)
{
    return fs::canonical(dir);
}

static std::string DirectoryLockKey(const fs::path& dir, const fs::path& lockfile_name)
{
    return fs::PathToString(dir / lockfile_name);
}

static bool TryMarkLocalDirectoryLock(const std::string& lock_key)
{
    LOCK(g_block_tree_store_locks_mutex);
    return g_block_tree_store_locks.insert(lock_key).second;
}

static void UnmarkLocalDirectoryLock(const std::string& lock_key)
{
    LOCK(g_block_tree_store_locks_mutex);
    g_block_tree_store_locks.erase(lock_key);
}

//! Blocks cross-process simultaneous read or write access to the data files.
//! Only a single instance may be created per directory at any one time.
class StoreAccessLock
{
    const fs::path m_dir;
    const std::string m_lock_key;

public:
    explicit StoreAccessLock(const fs::path& dir, bool wait = false, const util::SignalInterrupt* interrupt = nullptr)
        : m_dir{NormalizeDirectoryPath(dir)},
          m_lock_key{DirectoryLockKey(m_dir, STORE_ACCESS_LOCK_NAME)}
    {
        const std::chrono::milliseconds timeout{30s};
        const SteadyClock::time_point start{SteadyClock::now()};
        const auto check_wait{[&]() {
            if (interrupt && *interrupt) {
                throw BlockTreeStoreInterrupted("Interrupted waiting for block tree store access");
            }
            if (!wait && SteadyClock::now() > start + timeout) {
                throw BlockTreeStoreError(strprintf("Operation timed out waiting to acquire lock on %s", fs::PathToString(m_dir)));
            }
        }};
        for (;;) {
            if (!TryMarkLocalDirectoryLock(m_lock_key)) {
                check_wait();
                UninterruptibleSleep(1ms);
                continue;
            }
            try {
                switch (util::LockDirectory(m_dir, STORE_ACCESS_LOCK_NAME, /*probe_only=*/false, /*log_error=*/false)) {
                case util::LockResult::Success:
                    return;
                case util::LockResult::ErrorWrite:
                    throw BlockTreeStoreError(strprintf(
                        "Cannot create write-lock file in %s", fs::PathToString(m_dir)));
                case util::LockResult::ErrorLock: {
                    check_wait();
                    break;
                }
                }
            } catch (...) {
                UnmarkLocalDirectoryLock(m_lock_key);
                throw;
            }
            UnmarkLocalDirectoryLock(m_lock_key);
            // Read and write access is typically short, so wait a bit and try again.
            UninterruptibleSleep(1ms);
        }
    }

    ~StoreAccessLock()
    {
        UnlockDirectory(m_dir, STORE_ACCESS_LOCK_NAME);
        UnmarkLocalDirectoryLock(m_lock_key);
    }

    StoreAccessLock(const StoreAccessLock&) = delete;
    StoreAccessLock& operator=(const StoreAccessLock&) = delete;
};

/** A wrapper for creating a constant-sized serialization without varint encoding */
struct BlockFileInfoWrapper : CBlockFileInfo {
    static constexpr size_t SERIALIZED_SIZE{36};

    BlockFileInfoWrapper() = default;

    explicit BlockFileInfoWrapper(const CBlockFileInfo* info) : CBlockFileInfo(*info)
    {
    }

    SERIALIZE_METHODS(BlockFileInfoWrapper, obj)
    {
        READWRITE(obj.nBlocks);
        READWRITE(obj.nSize);
        READWRITE(obj.nUndoSize);
        READWRITE(obj.nHeightFirst);
        READWRITE(obj.nHeightLast);
        READWRITE(obj.nTimeFirst);
        READWRITE(obj.nTimeLast);
    }
};

WriterLock::WriterLock(const fs::path& dir)
    : m_dir{NormalizeDirectoryPath(dir)},
      m_lock_key{DirectoryLockKey(m_dir, WRITER_LOCK_NAME)}
{
    if (!TryMarkLocalDirectoryLock(m_lock_key)) {
        throw BlockTreeStoreError(strprintf(
            "This process is already writing to the block tree store in %s.", fs::PathToString(m_dir)));
    }
    try {
        switch (util::LockDirectory(m_dir, WRITER_LOCK_NAME)) {
        case util::LockResult::Success:
            return;
        case util::LockResult::ErrorWrite:
            throw BlockTreeStoreError(strprintf(
                "Cannot create writer-lock file in %s", fs::PathToString(m_dir)));
        case util::LockResult::ErrorLock:
            throw BlockTreeStoreError(strprintf(
                "Another process is already writing to the block tree store in %s.", fs::PathToString(m_dir)));
        }
    } catch (...) {
        UnmarkLocalDirectoryLock(m_lock_key);
        throw;
    }
    assert(0);
}

WriterLock::~WriterLock()
{
    UnlockDirectory(m_dir, WRITER_LOCK_NAME);
    UnmarkLocalDirectoryLock(m_lock_key);
}

static FilePosition CalculateBlockFileInfoPosition(int file_index)
{
    assert(file_index >= 0);
    return BLOCK_FILES_FILE_DATA_START_POSITION + file_index * (BlockFileInfoWrapper::SERIALIZED_SIZE + sizeof(Checksum));
}

const fs::path& BlockTreeStore::GetDataFilePath(ValueType value_type) const
{
    switch (value_type) {
    case ValueType::BLOCK_FILE_INFO:
        return m_block_files_file_path;
    case ValueType::DISK_BLOCK_INDEX:
        return m_header_file_path;
    }
    assert(false);
}

static uint8_t ValueSize(const ValueType value_type)
{
    switch (value_type) {
    case ValueType::BLOCK_FILE_INFO:
        return BlockFileInfoWrapper::SERIALIZED_SIZE;
    case ValueType::DISK_BLOCK_INDEX:
        return DiskBlockIndexWrapper::SERIALIZED_SIZE;
    }
    assert(false);
}

static FilePosition DataStartPosition(const ValueType value_type)
{
    switch (value_type) {
    case ValueType::BLOCK_FILE_INFO:
        return BLOCK_FILES_FILE_DATA_START_POSITION;
    case ValueType::DISK_BLOCK_INDEX:
        return HEADER_FILE_DATA_START_POSITION;
    }
    assert(false);
}

static FilePosition ValueRecordSize(const ValueType value_type)
{
    return ValueSize(value_type) + sizeof(Checksum);
}

static void CheckValuePosition(const ValueType value_type, const FilePosition position)
{
    const FilePosition start_position{DataStartPosition(value_type)};
    const FilePosition record_size{ValueRecordSize(value_type)};
    if (position < start_position || (position - start_position) % record_size != 0) {
        throw BlockTreeStoreError("Invalid target position in block tree store write-ahead log");
    }
}

static FilePosition RoundedDataFileEndPosition(const ValueType value_type, const FilePosition position)
{
    const FilePosition start_position{DataStartPosition(value_type)};
    const FilePosition record_size{ValueRecordSize(value_type)};
    if (position < start_position) {
        throw BlockTreeStoreError("Invalid block tree store data file size");
    }
    return position - (position - start_position) % record_size;
}

static void CheckPartialTailRecovered(const FilePosition original_end_position, const FilePosition virtual_end_position)
{
    if (virtual_end_position < original_end_position) {
        throw BlockTreeStoreError("Block tree store write-ahead log does not repair a partial data file record");
    }
}

static void CheckLogTargetPosition(const ValueType value_type, const FilePosition position, FilePosition& virtual_end_position)
{
    CheckValuePosition(value_type, position);
    if (position > virtual_end_position) {
        throw BlockTreeStoreError("Invalid sparse target position in block tree store write-ahead log");
    }
    if (position == virtual_end_position) {
        const FilePosition record_size{ValueRecordSize(value_type)};
        if (virtual_end_position > std::numeric_limits<FilePosition>::max() - record_size) {
            throw BlockTreeStoreError("Invalid target position overflow in block tree store write-ahead log");
        }
        virtual_end_position += record_size;
    }
}

static ValueType ReadValueType(AutoFile& file)
{
    std::underlying_type_t<ValueType> raw;
    file >> raw;
    switch (auto value_type{static_cast<ValueType>(raw)}) {
    case kernel::ValueType::BLOCK_FILE_INFO:
    case kernel::ValueType::DISK_BLOCK_INDEX:
        return value_type;
    }
    throw BlockTreeStoreError(strprintf("Unrecognized value type (%u) in block tree store", raw));
}

static void WriteMagicAndVersion(AutoFile& file, uint32_t magic, uint32_t version)
{
    file << magic;
    file << version;
}

static AutoFile OpenFile(const fs::path& path, const std::string& mode)
{
    AutoFile file{fsbridge::fopen(path, mode.c_str())};
    if (file.IsNull()) {
        throw BlockTreeStoreError(strprintf("Unable to open file %s", fs::PathToString(path)));
    }
    return AutoFile{file.release()};
}

static void CreateDataFile(const fs::path& path, uint32_t magic, uint32_t version)
{
    auto file{OpenFile(path, "wb")};

    WriteMagicAndVersion(file, magic, version);

    if (!file.Commit()) {
        throw BlockTreeStoreError(strprintf("Failed to write file %s", fs::PathToString(path)));
    }
    if (file.fclose() != 0) {
        throw BlockTreeStoreError(strprintf("Failed to close after write to file %s", fs::PathToString(path)));
    }
}

static AutoFile OpenFileAndVerifyHeader(const fs::path& path, uint32_t magic_expected, uint32_t version_expected, const std::string& mode = "rb")
{
    auto file{OpenFile(path, mode)};
    try {
        if (auto magic{ser_readdata32(file)}; magic != magic_expected) {
            throw BlockTreeStoreError(strprintf("Invalid magic in %s: 0x%08x (expected: 0x%08x)", fs::PathToString(path), magic, magic_expected));
        }
        if (auto version{ser_readdata32(file)}; version != version_expected) {
            throw BlockTreeStoreError(strprintf("Invalid version in %s: 0x%08x (expected: 0x%08x)", fs::PathToString(path), version, version_expected));
        }
    } catch (const std::ios_base::failure& e) {
        throw BlockTreeStoreError(strprintf("Unable to read file header %s: %s", fs::PathToString(path), e.what()));
    }
    return AutoFile{file.release()};
}

static bool DataFileIsHeaderOnly(const fs::path& path, uint32_t magic_expected, uint32_t version_expected, int64_t data_start_position)
{
    auto file{OpenFileAndVerifyHeader(path, magic_expected, version_expected)};
    return file.size() == data_start_position;
}

static Checksum CalculateFlagChecksum(uint8_t value)
{
    std::array<std::byte, sizeof(value)> buffer{std::byte{value}};
    return crc32c::Crc32c(UCharCast(buffer.data()), buffer.size());
}

static void WriteFlagFileRecord(AutoFile& file, bool value)
{
    const uint8_t raw_value{static_cast<uint8_t>(value ? 1 : 0)};
    file << raw_value;
    file << CalculateFlagChecksum(raw_value);
}

static bool ReadFlagFileRecord(AutoFile& file)
{
    uint8_t raw_value;
    Checksum stored_checksum;
    file >> raw_value;
    file >> stored_checksum;
    if (raw_value > 1 || stored_checksum != CalculateFlagChecksum(raw_value)) {
        throw BlockTreeStoreError("Invalid flag file record");
    }
    return raw_value == 1;
}

static void CreateFlagFile(const fs::path& path, bool value = false)
{
    auto file{OpenFile(path, "wb")};
    WriteMagicAndVersion(file, FLAG_FILE_MAGIC, FLAG_FILE_VERSION);
    WriteFlagFileRecord(file, value);
    if (!file.Commit()) {
        throw BlockTreeStoreError(strprintf("Failed to write flag file %s", fs::PathToString(path)));
    }
    if (file.fclose() != 0) {
        throw BlockTreeStoreError(strprintf("Failed to close flag file %s", fs::PathToString(path)));
    }
    if (!DirectoryCommit(path.parent_path())) {
        throw BlockTreeStoreError(strprintf("Failed to commit flag file directory %s", fs::PathToString(path.parent_path())));
    }
}

static std::optional<bool> ReadFlagFile(const fs::path& path)
{
    if (!fs::exists(path)) return std::nullopt;

    try {
        auto file{OpenFileAndVerifyHeader(path, FLAG_FILE_MAGIC, FLAG_FILE_VERSION)};
        const int64_t expected_size{FLAG_FILE_DATA_START_POSITION + FLAG_FILE_RECORD_SIZE};
        if (file.size() != expected_size) {
            throw BlockTreeStoreError(strprintf("Invalid flag file size in %s", fs::PathToString(path)));
        }

        return ReadFlagFileRecord(file);
    } catch (const std::ios_base::failure& e) {
        throw BlockTreeStoreError(strprintf("Unable to read flag file %s: %s", fs::PathToString(path), e.what()));
    }
}

static bool MissingOrHeaderOnlyLogFile(const fs::path& path)
{
    return !fs::exists(path) || DataFileIsHeaderOnly(path, LOG_FILE_MAGIC, LOG_FILE_VERSION, LOG_FILE_DATA_START_POSITION);
}

static bool SafeToCreateInitialFlags(const fs::path& log_path)
{
    try {
        return MissingOrHeaderOnlyLogFile(log_path);
    } catch (const BlockTreeStoreError&) {
        return false;
    }
}

static void EnsureLogFile(const fs::path& path)
{
    if (fs::exists(path)) return;
    CreateDataFile(path, LOG_FILE_MAGIC, LOG_FILE_VERSION);
    if (!DirectoryCommit(path.parent_path())) {
        throw BlockTreeStoreError(strprintf("Failed to commit log file directory %s", fs::PathToString(path.parent_path())));
    }
}

static void EnsureFlagFile(const fs::path& path, bool allow_create)
{
    try {
        if (ReadFlagFile(path)) return;
    } catch (const BlockTreeStoreError&) {
        if (!allow_create) throw;
    }
    if (!allow_create) {
        throw BlockTreeStoreError(strprintf("Missing flag file %s", fs::PathToString(path)));
    }
    CreateFlagFile(path);
}

static bool ReadFlag(const fs::path& path)
{
    auto value{ReadFlagFile(path)};
    if (!value) {
        throw BlockTreeStoreError(strprintf("Missing flag file %s", fs::PathToString(path)));
    }
    return *value;
}

BlockTreeStore::BlockTreeStore(const fs::path& path, const OpenMode open_mode, const util::SignalInterrupt* interrupt)
    : m_header_file_path{path / HEADER_FILE_NAME},
      m_log_file_path{path / LOG_FILE_NAME},
      m_log_flag_file_path{path / LOG_FLAG_FILE_NAME},
      m_block_files_file_path{path / BLOCK_FILES_FILE_NAME},
      m_reindex_flag_file_path{path / REINDEX_FLAG_FILE_NAME},
      m_prune_flag_file_path{path / PRUNE_FLAG_FILE_NAME},
      m_mode{open_mode},
      m_interrupt{interrupt}
{
    assert(GetSerializeSize(DiskBlockIndexWrapper{}) == DiskBlockIndexWrapper::SERIALIZED_SIZE);
    assert(GetSerializeSize(BlockFileInfoWrapper{}) == BlockFileInfoWrapper::SERIALIZED_SIZE);

    if (m_mode == OpenMode::READ) return;

    LOCK(m_mutex);
    fs::create_directories(path);
    m_writer_lock.emplace(path);
    StoreAccessLock access_lock{m_header_file_path.parent_path(), /*wait=*/true, m_interrupt};

    if (m_mode == OpenMode::WIPE) {
        fs::remove(m_header_file_path);
        fs::remove(m_block_files_file_path);
        fs::remove(m_log_file_path);
        fs::remove(m_log_flag_file_path);
        fs::remove(m_reindex_flag_file_path);
        fs::remove(m_prune_flag_file_path);
    }
    bool header_file_exists{fs::exists(m_header_file_path)};
    bool block_files_file_exists{fs::exists(m_block_files_file_path)};
    if (header_file_exists && !block_files_file_exists) {
        if (!DataFileIsHeaderOnly(m_header_file_path, HEADER_FILE_MAGIC, HEADER_FILE_VERSION, HEADER_FILE_DATA_START_POSITION)) {
            throw BlockTreeStoreError("Block tree store is in an inconsistent state");
        }
        CreateDataFile(m_block_files_file_path, BLOCK_FILES_FILE_MAGIC, BLOCK_FILES_FILE_VERSION);
        block_files_file_exists = true;
    } else if (!header_file_exists && block_files_file_exists) {
        if (!DataFileIsHeaderOnly(m_block_files_file_path, BLOCK_FILES_FILE_MAGIC, BLOCK_FILES_FILE_VERSION, BLOCK_FILES_FILE_DATA_START_POSITION)) {
            throw BlockTreeStoreError("Block tree store is in an inconsistent state");
        }
        CreateDataFile(m_header_file_path, HEADER_FILE_MAGIC, HEADER_FILE_VERSION);
        header_file_exists = true;
    }
    if (!header_file_exists && !block_files_file_exists) {
        CreateDataFile(m_header_file_path, HEADER_FILE_MAGIC, HEADER_FILE_VERSION);
        CreateDataFile(m_block_files_file_path, BLOCK_FILES_FILE_MAGIC, BLOCK_FILES_FILE_VERSION);
    }
    (void)OpenFileAndVerifyHeader(m_header_file_path, HEADER_FILE_MAGIC, HEADER_FILE_VERSION);
    (void)OpenFileAndVerifyHeader(m_block_files_file_path, BLOCK_FILES_FILE_MAGIC, BLOCK_FILES_FILE_VERSION);
    const bool store_empty{
        DataFileIsHeaderOnly(m_header_file_path, HEADER_FILE_MAGIC, HEADER_FILE_VERSION, HEADER_FILE_DATA_START_POSITION) &&
        DataFileIsHeaderOnly(m_block_files_file_path, BLOCK_FILES_FILE_MAGIC, BLOCK_FILES_FILE_VERSION, BLOCK_FILES_FILE_DATA_START_POSITION)};
    const bool safe_to_create_initial_flags{store_empty && SafeToCreateInitialFlags(m_log_file_path)};
    EnsureFlagFile(m_reindex_flag_file_path, safe_to_create_initial_flags);
    EnsureFlagFile(m_prune_flag_file_path, safe_to_create_initial_flags);

    const auto recover_empty_log_flag{[&]() {
        if (!safe_to_create_initial_flags) return false;
        CreateFlagFile(m_log_flag_file_path);
        EnsureLogFile(m_log_file_path);
        return true;
    }};
    std::optional<bool> log_pending;
    try {
        log_pending = ReadFlagFile(m_log_flag_file_path);
    } catch (const BlockTreeStoreError&) {
    }
    if (!log_pending.has_value()) {
        if (!recover_empty_log_flag()) {
            if (!ApplyLog(/*force_recovery=*/true)) {
                throw BlockTreeStoreError("Failed to recover block tree store write-ahead log from an invalid flag");
            }
            LogInfo("Recovered block tree store write-ahead log from an invalid or missing completion flag.");
        }
    } else {
        if (!*log_pending) {
            EnsureLogFile(m_log_file_path);
        }
        if (ApplyLog()) {
            LogInfo("Applied block tree store write-ahead log left over from a previous failure, potentially caused by unclean shutdown or intermittent hardware issue.");
        }
    }
}

void BlockTreeStore::CheckWriteAccess() const
{
    if (m_mode == OpenMode::READ) throw std::logic_error("Block tree store writes are disabled when opened in read mode");
}

void BlockTreeStore::WriteFlag(const fs::path& path, bool value, bool directory_commit) const
{
    if (!ReadFlagFile(path).has_value()) {
        throw BlockTreeStoreError(strprintf("Missing flag file %s", fs::PathToString(path)));
    }
    const bool replace{path != m_log_flag_file_path};
    fs::path write_path{path};
    if (replace) write_path += ".tmp";
    auto file{OpenFile(write_path, replace ? "wb" : "rb+")};
    if (replace) {
        WriteMagicAndVersion(file, FLAG_FILE_MAGIC, FLAG_FILE_VERSION);
        // TEST ONLY: leave a torn temporary record without touching the published flag.
        if (m_incomplete_flag_write) {
            file << static_cast<uint8_t>(value ? 1 : 0);
            (void)file.fclose();
            throw std::runtime_error("failed to write flag");
        }
    } else {
        file.seek(FLAG_FILE_DATA_START_POSITION, SEEK_SET);
    }
    WriteFlagFileRecord(file, value);

    // TEST ONLY
    if (!replace && value && m_incomplete_log_flag_commit) {
        (void)file.fclose();
        throw std::runtime_error("failed to commit log flag");
    }

    if (!file.Commit()) {
        throw BlockTreeStoreError(strprintf("Could not write flag file %s", fs::PathToString(write_path)));
    }
    if (file.fclose() != 0) {
        throw BlockTreeStoreError(strprintf("Could not close flag file %s", fs::PathToString(write_path)));
    }
    if (replace && !RenameOver(write_path, path)) {
        throw BlockTreeStoreError(strprintf("Could not replace flag file %s", fs::PathToString(path)));
    }
    if (directory_commit && !DirectoryCommit(path.parent_path())) {
        throw BlockTreeStoreError(strprintf("Could not commit flag file directory %s", fs::PathToString(path.parent_path())));
    }
}

void BlockTreeStore::ReadReindexing(bool& reindexing) const
{
    LOCK(m_mutex);
    StoreAccessLock lock_file{m_log_file_path.parent_path(), /*wait=*/m_mode != OpenMode::READ, m_interrupt};
    reindexing = ReadFlag(m_reindex_flag_file_path);
}

void BlockTreeStore::WriteReindexing(bool reindexing) const
{
    CheckWriteAccess();
    LOCK(m_mutex);
    StoreAccessLock lock_file{m_log_file_path.parent_path(), /*wait=*/true, m_interrupt};
    WriteFlag(m_reindex_flag_file_path, /*value=*/reindexing, /*directory_commit=*/true);
}

void BlockTreeStore::ReadLastBlockFile(int32_t& last_block_file) const
{
    LOCK(m_mutex);
    StoreAccessLock lock_file{m_log_file_path.parent_path(), /*wait=*/m_mode != OpenMode::READ, m_interrupt};
    if (ReadFlag(m_log_flag_file_path)) {
        throw BlockTreeStoreError("Cannot read block tree store while a write-ahead log is pending");
    }
    auto file{OpenFileAndVerifyHeader(m_block_files_file_path, BLOCK_FILES_FILE_MAGIC, BLOCK_FILES_FILE_VERSION)};

    constexpr uint64_t entry_size = BlockFileInfoWrapper::SERIALIZED_SIZE + sizeof(Checksum);
    const int64_t file_data_size{file.size() - BLOCK_FILES_FILE_DATA_START_POSITION};
    if (file_data_size < 0 || file_data_size % entry_size != 0) {
        throw BlockTreeStoreError("Invalid block files file data");
    }
    last_block_file = file_data_size == 0 ? 0 : file_data_size / entry_size - 1;
}

void BlockTreeStore::ReadPruned(bool& pruned) const
{
    LOCK(m_mutex);
    StoreAccessLock lock_file{m_log_file_path.parent_path(), /*wait=*/m_mode != OpenMode::READ, m_interrupt};
    pruned = ReadFlag(m_prune_flag_file_path);
}

void BlockTreeStore::WritePruned(bool pruned) const
{
    CheckWriteAccess();
    LOCK(m_mutex);
    StoreAccessLock lock_file{m_log_file_path.parent_path(), /*wait=*/true, m_interrupt};
    WriteFlag(m_prune_flag_file_path, /*value=*/pruned, /*directory_commit=*/true);
}

static Checksum ExtendChecksum(Checksum checksum, std::span<const std::byte> value_data, FilePosition position)
{
    checksum = crc32c::Extend(checksum, UCharCast(value_data.data()), value_data.size());
    std::array<std::byte, sizeof(FilePosition)> position_bytes;
    WriteLE64(UCharCast(position_bytes.data()), static_cast<uint64_t>(position));
    return crc32c::Extend(checksum, UCharCast(position_bytes.data()), position_bytes.size());
}

static Checksum SingleChecksum(std::span<const std::byte> value_data, FilePosition position)
{
    return ExtendChecksum(0, value_data, position);
}

static void WriteLogFileSectionHeader(AutoFile& log_file, ValueType value_type, uint64_t record_count)
{
    log_file << static_cast<std::underlying_type_t<ValueType>>(value_type);
    log_file << record_count;
}

static std::pair<ValueType, uint64_t> ReadLogFileSectionHeader(AutoFile& log_file)
{
    const ValueType value_type{ReadValueType(log_file)};
    uint64_t record_count;
    log_file >> record_count;
    return {value_type, record_count};
}

struct LogFileRecord {
    ValueType m_value_type;
    std::vector<std::byte> m_value_buffer;
    FilePosition m_position;
    Checksum m_checksum;

    LogFileRecord(ValueType value_type) : m_value_type{value_type}, m_value_buffer(ValueSize(value_type)) {}
};

static void ReadLogFileRecord(AutoFile& log_file, LogFileRecord& record, Checksum& rolling_checksum)
{
    log_file.read(record.m_value_buffer);
    log_file >> record.m_position;
    CheckValuePosition(record.m_value_type, record.m_position);

    record.m_checksum = SingleChecksum(record.m_value_buffer, record.m_position);
    rolling_checksum = ExtendChecksum(rolling_checksum, record.m_value_buffer, record.m_position);

    Checksum stored_checksum;
    log_file >> stored_checksum;
    if (stored_checksum != record.m_checksum) {
        throw BlockTreeStoreError("Detected on-disk log file corruption: Checksum mismatch");
    }
}

template <typename Wrapper>
static void WriteLogFileRecord(AutoFile& log_file, const Wrapper& wrapper, FilePosition position, Checksum& rolling_checksum)
{
    std::array<std::byte, Wrapper::SERIALIZED_SIZE> value_buffer;
    SpanWriter{value_buffer} << wrapper;
    const Checksum checksum{SingleChecksum(value_buffer, position)};
    rolling_checksum = ExtendChecksum(rolling_checksum, value_buffer, position);
    log_file.write(value_buffer);
    log_file << position;
    log_file << checksum;
}

static void ReadDataValue(AutoFile& file, std::span<std::byte> value_buffer)
{
    const FilePosition position{file.tell()};
    file.read(value_buffer);
    Checksum checksum;
    file >> checksum;
    if (SingleChecksum(value_buffer, position) != checksum) {
        throw BlockTreeStoreError("Record data failed integrity check");
    }
}

bool BlockTreeStore::ReadBlockFileInfo(int file_index, CBlockFileInfo& info)
{
    LOCK(m_mutex);
    StoreAccessLock lock_file{m_log_file_path.parent_path(), /*wait=*/m_mode != OpenMode::READ, m_interrupt};
    if (ReadFlag(m_log_flag_file_path)) {
        throw BlockTreeStoreError("Cannot read block tree store while a write-ahead log is pending");
    }

    auto file{OpenFileAndVerifyHeader(m_block_files_file_path, BLOCK_FILES_FILE_MAGIC, BLOCK_FILES_FILE_VERSION)};
    constexpr uint64_t entry_size = BlockFileInfoWrapper::SERIALIZED_SIZE + sizeof(Checksum);
    const int64_t file_data_size{file.size() - BLOCK_FILES_FILE_DATA_START_POSITION};
    if (file_data_size < 0 || file_data_size % entry_size != 0) {
        throw BlockTreeStoreError("Invalid block files file data");
    }
    if (file_index < 0 || std::cmp_greater_equal(file_index, file_data_size / entry_size)) {
        return false;
    }
    file.seek(CalculateBlockFileInfoPosition(file_index), SEEK_SET);

    BlockFileInfoWrapper info_wrapper;
    std::array<std::byte, BlockFileInfoWrapper::SERIALIZED_SIZE> buffer;
    try {
        ReadDataValue(file, buffer);
        SpanReader{buffer} >> info_wrapper;
    } catch (const std::ios_base::failure& e) {
        throw BlockTreeStoreError(strprintf("Unable to read block file info record %i: %s", file_index, e.what()));
    }

    info = info_wrapper;
    return true;
}

bool BlockTreeStore::ApplyLog(bool force_recovery, bool log_flag_committed) const
{
    AssertLockHeld(m_mutex);

    if (!force_recovery && !ReadFlag(m_log_flag_file_path)) {
        return false;
    }
    if (!fs::exists(m_log_file_path)) {
        throw BlockTreeStoreError("Missing completed block tree store write-ahead log");
    }

    auto log_file{OpenFileAndVerifyHeader(m_log_file_path, LOG_FILE_MAGIC, LOG_FILE_VERSION)};
    const FilePosition block_files_original_end{OpenFileAndVerifyHeader(m_block_files_file_path, BLOCK_FILES_FILE_MAGIC, BLOCK_FILES_FILE_VERSION).size()};
    FilePosition block_files_virtual_end{RoundedDataFileEndPosition(ValueType::BLOCK_FILE_INFO, block_files_original_end)};
    const FilePosition headers_original_end{OpenFileAndVerifyHeader(m_header_file_path, HEADER_FILE_MAGIC, HEADER_FILE_VERSION).size()};
    FilePosition headers_virtual_end{RoundedDataFileEndPosition(ValueType::DISK_BLOCK_INDEX, headers_original_end)};
    const auto virtual_end_position{[&](ValueType value_type) -> FilePosition& {
        switch (value_type) {
        case ValueType::BLOCK_FILE_INFO:
            return block_files_virtual_end;
        case ValueType::DISK_BLOCK_INDEX:
            return headers_virtual_end;
        }
        assert(false);
    }};

    Checksum rolling_checksum = 0;
    Checksum stored_rolling_checksum = 0;
    uint32_t number_of_types = 0;
    const FilePosition log_file_size{log_file.size()};

    // Do a dry run to check the integrity of the log file. This should help prevent cascading errors in case of log file corruption.
    try {
        log_file >> number_of_types;
        if (number_of_types > 2) {
            throw BlockTreeStoreError("Invalid number of sections in block tree store write-ahead log");
        }
        for (uint32_t i = 0; i < number_of_types; i++) {
            const auto [value_type, record_count] = ReadLogFileSectionHeader(log_file);
            const int64_t bytes_remaining{log_file.size() - log_file.tell() - static_cast<int64_t>(sizeof(stored_rolling_checksum))};
            if (bytes_remaining < 0 || std::cmp_greater(record_count, bytes_remaining / (ValueSize(value_type) + sizeof(FilePosition) + sizeof(Checksum)))) {
                throw BlockTreeStoreError("Invalid record count in block tree store write-ahead log");
            }
            LogFileRecord record{value_type};

            for (uint64_t j = 0; j < record_count; j++) {
                ReadLogFileRecord(log_file, record, rolling_checksum);
                CheckLogTargetPosition(value_type, record.m_position, virtual_end_position(value_type));
            }
        }

        log_file >> stored_rolling_checksum;
        if (rolling_checksum != stored_rolling_checksum) {
            throw BlockTreeStoreError("Detected on-disk log file corruption: Rolling checksum mismatch");
        }
        if (log_file.tell() != log_file_size) {
            throw BlockTreeStoreError("Detected on-disk log file corruption: Trailing data after checksum");
        }
        CheckPartialTailRecovered(block_files_original_end, block_files_virtual_end);
        CheckPartialTailRecovered(headers_original_end, headers_virtual_end);
    } catch (const std::ios_base::failure& e) {
        throw BlockTreeStoreError(strprintf("Encountered exception while checking log file: %s", e.what()));
    }

    if (force_recovery) {
        auto writable_log_file{OpenFileAndVerifyHeader(m_log_file_path, LOG_FILE_MAGIC, LOG_FILE_VERSION, "rb+")};
        if (!writable_log_file.Commit()) {
            throw BlockTreeStoreError(strprintf("Failed to commit write-ahead log file %s before recovery", fs::PathToString(m_log_file_path)));
        }
        if (writable_log_file.fclose() != 0) {
            throw BlockTreeStoreError(strprintf("Failed to close write-ahead log file %s before recovery", fs::PathToString(m_log_file_path)));
        }
        if (!DirectoryCommit(m_log_file_path.parent_path())) {
            throw BlockTreeStoreError(strprintf("Failed to commit log file directory %s before recovery", fs::PathToString(m_log_file_path.parent_path())));
        }
    }
    if (!log_flag_committed) {
        if (force_recovery) {
            CreateFlagFile(m_log_flag_file_path, /*value=*/true);
        } else {
            WriteFlag(m_log_flag_file_path, /*value=*/true, /*directory_commit=*/false);
        }
    }

    rolling_checksum = 0;
    stored_rolling_checksum = 0;
    // Seek back to the start of the log file data, but skip reading the number of types again
    log_file.seek(LOG_FILE_DATA_START_POSITION + sizeof(number_of_types), SEEK_SET);

    // Run through the file again, but this time write it to the target data files.
    for (uint32_t i = 0; i < number_of_types; ++i) {
        const auto [value_type, record_count] = ReadLogFileSectionHeader(log_file);
        auto data_file_path{GetDataFilePath(value_type)};
        auto data_file{OpenFile(data_file_path, "rb+")};
        LogFileRecord record{value_type};

        for (uint64_t j = 0; j < record_count; ++j) {
            ReadLogFileRecord(log_file, record, rolling_checksum);

            if (data_file.tell() != record.m_position) {
                data_file.seek(record.m_position, SEEK_SET);
            }

            data_file.write(record.m_value_buffer);
            data_file << record.m_checksum;

            // TEST ONLY
            if (m_incomplete_log_apply) {
                (void)data_file.fclose();
                return false;
            }
        }

        if (!data_file.Commit()) {
            throw BlockTreeStoreError(strprintf("Failed to commit write to data file %s", PathToString(data_file_path)));
        }
        if (data_file.fclose() != 0) {
            throw BlockTreeStoreError(strprintf("Failed to close after write to data file %s", PathToString(data_file_path)));
        }
    }

    log_file >> stored_rolling_checksum;
    if (rolling_checksum != stored_rolling_checksum) {
        throw BlockTreeStoreError("Detected on-disk log file corruption: Rolling checksum mismatch");
    }

    (void)log_file.fclose();
    // Reapplying a complete log after a later failure is idempotent, so avoid an unnecessary directory commit.
    WriteFlag(m_log_flag_file_path, /*value=*/false, /*directory_commit=*/false);
    return true;
}

void BlockTreeStore::WriteBatchSync(const std::vector<std::pair<int, const CBlockFileInfo*>>& file_infos_to_write, const std::vector<CBlockIndex*>& block_indexes_to_write)
{
    CheckWriteAccess();
    AssertLockHeld(::cs_main);
    LOCK(m_mutex);
    StoreAccessLock lock_file{m_log_file_path.parent_path(), /*wait=*/true, m_interrupt};

    // If there is a complete log waiting to be applied, write that first. An incomplete log is discarded.
    // This may occur if a previous write threw an exception when writing the logged data to the .dat files.
    for (const auto& [file, _] : file_infos_to_write) {
        if (file < 0) {
            throw BlockTreeStoreError("Cannot write a negative block file index to the block tree store");
        }
    }

    if (ApplyLog()) {
        LogInfo("Applied block tree store write-ahead log left over from a previous failure, potentially caused by unclean shutdown or intermittent hardware issue.");
    }

    if (file_infos_to_write.empty() && block_indexes_to_write.empty()) return;
    WriteFlag(m_log_flag_file_path, /*value=*/false, /*directory_commit=*/false);
    EnsureLogFile(m_log_file_path);

    std::vector<std::pair<CBlockIndex*, FilePosition>> pending_header_positions;
    pending_header_positions.reserve(block_indexes_to_write.size());

    // Use a write-ahead log file that gets applied to the target files.

    { // start log_file scope
    auto log_file{OpenFile(m_log_file_path, "wb")};
    WriteMagicAndVersion(log_file, LOG_FILE_MAGIC, LOG_FILE_VERSION);
    const uint32_t log_num_types{(file_infos_to_write.empty() || block_indexes_to_write.empty()) ? 1u : 2u};
    log_file << log_num_types;

    Checksum rolling_checksum = 0;

    // Write the file_info entries to the log
    if (!file_infos_to_write.empty()) {
        WriteLogFileSectionHeader(log_file, ValueType::BLOCK_FILE_INFO, file_infos_to_write.size());
        for (const auto& [file, info] : file_infos_to_write) {
            WriteLogFileRecord(log_file, BlockFileInfoWrapper{info}, CalculateBlockFileInfoPosition(file), rolling_checksum);
        }
    }

    // TEST ONLY
    if (m_incomplete_log_write) {
        (void)log_file.fclose();
        throw std::runtime_error("failed to write file");
    }

    if (!block_indexes_to_write.empty()) {
        // Read the header data end position
        FilePosition header_data_end;
        {
            auto header_file{OpenFileAndVerifyHeader(m_header_file_path, HEADER_FILE_MAGIC, HEADER_FILE_VERSION)};
            header_data_end = header_file.size();
        }

        // Write the block_indexes_to_write data to the log
        WriteLogFileSectionHeader(log_file, ValueType::DISK_BLOCK_INDEX, block_indexes_to_write.size());
        for (CBlockIndex* block_index : block_indexes_to_write) {
            FilePosition position = block_index->header_pos == CBlockIndex::UNSET_HEADER_POS ? header_data_end : block_index->header_pos;
            CDiskBlockIndex disk_index{block_index};
            WriteLogFileRecord(log_file, DiskBlockIndexWrapper{&disk_index}, position, rolling_checksum);
            if (block_index->header_pos == CBlockIndex::UNSET_HEADER_POS) {
                pending_header_positions.emplace_back(block_index, header_data_end);
                header_data_end += DiskBlockIndexWrapper::SERIALIZED_SIZE + sizeof(Checksum);
            }
        }
    }

    // Finally write the rolling checksum and commit.
    log_file << rolling_checksum;
    if (!log_file.Commit()) {
        throw BlockTreeStoreError(strprintf("Failed to commit write to log file %s", PathToString(m_log_file_path)));
    }
    if (log_file.fclose() != 0) {
        throw BlockTreeStoreError(strprintf("Failed to close after write to log file %s", PathToString(m_log_file_path)));
    }

    } // end log_file scope

    if (!DirectoryCommit(m_log_file_path.parent_path())) {
        throw BlockTreeStoreError(strprintf("Failed to commit log file directory %s", fs::PathToString(m_log_file_path.parent_path())));
    }

    // Write the flag indicating log file completion (which also executes CommitDirectory)
    WriteFlag(m_log_flag_file_path, /*value=*/true, /*directory_commit=*/true);

    // Once committed, apply the header positions to the index and close the file.
    for (const auto& [block_index, header_pos] : pending_header_positions) {
        block_index->header_pos = header_pos;
    }

    if (!ApplyLog(/*force_recovery=*/false, /*log_flag_committed=*/true)) {
        throw BlockTreeStoreError("Failed to apply write-ahead log to data files");
    }
}

bool BlockTreeStore::LoadBlockIndexGuts(
    const Consensus::Params& consensus_params,
    std::function<CBlockIndex*(const uint256&)> insert_block_index,
    const util::SignalInterrupt& interrupt)
{
    AssertLockHeld(::cs_main);
    LOCK(m_mutex);
    StoreAccessLock lock_file{m_log_file_path.parent_path(), /*wait=*/m_mode != OpenMode::READ, m_interrupt};
    if (ReadFlag(m_log_flag_file_path)) {
        throw BlockTreeStoreError("Cannot read block tree store while a write-ahead log is pending");
    }

    auto file{OpenFileAndVerifyHeader(m_header_file_path, HEADER_FILE_MAGIC, HEADER_FILE_VERSION)};

    FilePosition data_end_position = file.size();
    file.seek(HEADER_FILE_DATA_START_POSITION, SEEK_SET);

    DiskBlockIndexWrapper disk_index;
    std::array<std::byte, DiskBlockIndexWrapper::SERIALIZED_SIZE> buffer;

    while (file.tell() < data_end_position) {
        if (interrupt) return false;

        auto record_start{file.tell()};
        ReadDataValue(file, buffer);
        SpanReader{buffer} >> disk_index;

        // Construct block index object
        CBlockIndex* block_index = insert_block_index(disk_index.ConstructBlockHash());
        block_index->pprev = insert_block_index(disk_index.hashPrev);
        block_index->header_pos = record_start;
        block_index->nHeight = disk_index.nHeight;
        block_index->nFile = disk_index.nFile;
        block_index->nDataPos = disk_index.nDataPos;
        block_index->nUndoPos = disk_index.nUndoPos;
        block_index->nVersion = disk_index.nVersion;
        block_index->hashMerkleRoot = disk_index.hashMerkleRoot;
        block_index->nTime = disk_index.nTime;
        block_index->nBits = disk_index.nBits;
        block_index->nNonce = disk_index.nNonce;
        block_index->nStatus = disk_index.nStatus;
        block_index->nTx = disk_index.nTx;

        if (!CheckProofOfWork(block_index->GetBlockHash(), block_index->nBits, consensus_params)) {
            LogError("CheckProofOfWork failed: %s", block_index->ToString());
            return false;
        }
    }

    return true;
}

} // namespace kernel
