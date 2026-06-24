// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <crc32c/include/crc32c/crc32c.h>
#include <crypto/common.h>
#include <kernel/blocktreestorage.h>
#include <logging.h>
#include <node/blockstorage.h>
#include <pow.h>
#include <primitives/block.h>
#include <streams.h>
#include <test/util/setup_common.h>
#include <util/fs_helpers.h>
#include <util/hasher.h>

#include <boost/test/unit_test.hpp>

#include <array>
#include <span>
#include <type_traits>

using kernel::BLOCK_FILES_FILE_DATA_START_POSITION;
using kernel::BLOCK_FILES_FILE_MAGIC;
using kernel::BLOCK_FILES_FILE_NAME;
using kernel::BLOCK_FILES_FILE_VERSION;
using kernel::BlockTreeStore;
using kernel::BlockTreeStoreError;
using kernel::CBlockFileInfo;
using kernel::HEADER_FILE_DATA_START_POSITION;
using kernel::HEADER_FILE_MAGIC;
using kernel::HEADER_FILE_NAME;
using kernel::HEADER_FILE_VERSION;
using kernel::LOG_FILE_DATA_START_POSITION;
using kernel::LOG_FILE_MAGIC;
using kernel::LOG_FILE_NAME;
using kernel::LOG_FILE_VERSION;
using kernel::LOG_FLAG_FILE_NAME;
using kernel::PRUNE_FLAG_FILE_NAME;
using kernel::REINDEX_FLAG_FILE_NAME;
using kernel::ValueType;

BOOST_FIXTURE_TEST_SUITE(blocktreestorage_tests, BasicTestingSetup)

static constexpr int64_t BLOCK_FILE_INFO_RECORD_SIZE{36 + sizeof(uint32_t)};

CBlockIndex* InsertBlockIndex(node::BlockMap& block_map, const uint256& hash)
{
    if (hash.IsNull()) {
        return nullptr;
    }
    const auto [mi, inserted]{block_map.try_emplace(hash)};
    CBlockIndex* pindex = &(*mi).second;
    if (inserted) {
        pindex->phashBlock = &((*mi).first);
    }
    return pindex;
}

CBlockFileInfo CreateFileInfo(int32_t seed)
{
    CBlockFileInfo info;
    info.nBlocks = seed;
    info.nSize = seed + 1;
    info.nUndoSize = seed + 2;
    info.nHeightFirst = seed + 3;
    info.nHeightLast = seed + 4;
    info.nTimeFirst = seed + 5;
    info.nTimeLast = seed + 6;
    return info;
}

struct ExpectedStoreState {
    std::map<int, CBlockFileInfo> file_infos{};
    node::BlockMap block_map{};
    bool pruned{false};
    bool reindexing{false};
    util::SignalInterrupt& interrupt;
    const CChainParams& params;
};

void CheckBlockFileInfo(uint32_t file, CBlockFileInfo& file_info, BlockTreeStore& store)
{
    CBlockFileInfo retrieved_info;
    BOOST_CHECK(store.ReadBlockFileInfo(file, retrieved_info));
    BOOST_CHECK_EQUAL(file_info.nBlocks, retrieved_info.nBlocks);
    BOOST_CHECK_EQUAL(file_info.nSize, retrieved_info.nSize);
    BOOST_CHECK_EQUAL(file_info.nUndoSize, retrieved_info.nUndoSize);
    BOOST_CHECK_EQUAL(file_info.nHeightFirst, retrieved_info.nHeightFirst);
    BOOST_CHECK_EQUAL(file_info.nHeightLast, retrieved_info.nHeightLast);
    BOOST_CHECK_EQUAL(file_info.nTimeFirst, retrieved_info.nTimeFirst);
    BOOST_CHECK_EQUAL(file_info.nTimeLast, retrieved_info.nTimeLast);

    DataStream a, b;
    a << file_info;
    b << retrieved_info;
    BOOST_CHECK_EQUAL(a.str(), b.str());
}

void CheckBlockMap(const node::BlockMap& store_block_map, const node::BlockMap& expected_block_map)
{
    LOCK(::cs_main);
    BOOST_CHECK_EQUAL(store_block_map.size(), expected_block_map.size());
    for (const auto& [block_hash, store_index] : store_block_map) {
        auto it = expected_block_map.find(block_hash);
        BOOST_REQUIRE(it != expected_block_map.end());
        const auto& expected_index = it->second;

        BOOST_CHECK_EQUAL(expected_index.nHeight, store_index.nHeight);
        BOOST_CHECK_EQUAL(expected_index.nStatus, store_index.nStatus);
        BOOST_CHECK_EQUAL(expected_index.nTx, store_index.nTx);
        BOOST_CHECK_EQUAL(expected_index.nFile, store_index.nFile);
        BOOST_CHECK_EQUAL(expected_index.nDataPos, store_index.nDataPos);
        BOOST_CHECK_EQUAL(expected_index.nUndoPos, store_index.nUndoPos);

        BOOST_CHECK_EQUAL(expected_index.nVersion, store_index.nVersion);
        if (expected_index.pprev == nullptr || store_index.pprev == nullptr) {
            BOOST_CHECK_EQUAL(expected_index.pprev, store_index.pprev);
        } else {
            BOOST_CHECK_EQUAL(Assert(expected_index.pprev)->GetBlockHeader().GetHash().ToString(), Assert(store_index.pprev)->GetBlockHeader().GetHash().ToString());
        }
        BOOST_CHECK_EQUAL(expected_index.hashMerkleRoot.ToString(), store_index.hashMerkleRoot.ToString());
        BOOST_CHECK_EQUAL(expected_index.nTime, store_index.nTime);
        BOOST_CHECK_EQUAL(expected_index.nBits, store_index.nBits);
        BOOST_CHECK_EQUAL(expected_index.nNonce, store_index.nNonce);

        DataStream store, expected;
        store << CDiskBlockIndex{&store_index};
        expected << CDiskBlockIndex{&expected_index};
        BOOST_CHECK_EQUAL(store.str(), expected.str());
    }
}

void CheckStoreContents(BlockTreeStore& store,
                        const ExpectedStoreState& expected_state,
                        const std::string& context)
{
    BOOST_TEST_CONTEXT(context)
    {
        for (auto& [file, info] : expected_state.file_infos) {
            CBlockFileInfo copy{info};
            CheckBlockFileInfo(file, copy, store);
        }
        int32_t last_block;
        store.ReadLastBlockFile(last_block);
        int32_t expected_last = expected_state.file_infos.empty() ? 0 : expected_state.file_infos.size() - 1;
        BOOST_CHECK_EQUAL(last_block, expected_last);

        LOCK(::cs_main);
        node::BlockMap block_map;
        BOOST_CHECK(store.LoadBlockIndexGuts(
            expected_state.params.GetConsensus(),
            [&](const uint256& hash) { return InsertBlockIndex(block_map, hash); },
            expected_state.interrupt));
        CheckBlockMap(block_map, expected_state.block_map);

        bool pruned, reindexing;
        store.ReadPruned(pruned);
        store.ReadReindexing(reindexing);
        BOOST_CHECK_EQUAL(pruned, expected_state.pruned);
        BOOST_CHECK_EQUAL(reindexing, expected_state.reindexing);
    }
}

std::vector<CBlockIndex*> BlockMapToVector(node::BlockMap& test_block_map)
{
    std::vector<CBlockIndex*> blocks;
    blocks.reserve(test_block_map.size());
    for (auto& [hash, index] : test_block_map) {
        blocks.push_back(&index);
    }
    return blocks;
}

std::vector<std::pair<int, const CBlockFileInfo*>> FileInfosToPairs(std::span<CBlockFileInfo> file_infos)
{
    std::vector<std::pair<int, const CBlockFileInfo*>> pairs;
    pairs.reserve(file_infos.size());
    for (size_t i{0}; i < file_infos.size(); ++i) {
        pairs.emplace_back(static_cast<int>(i), &file_infos[i]);
    }
    return pairs;
}

std::vector<std::pair<int, const CBlockFileInfo*>> FileInfosToPairs(const std::map<int, CBlockFileInfo>& file_infos)
{
    std::vector<std::pair<int, const CBlockFileInfo*>> pairs;
    pairs.reserve(file_infos.size());
    for (const auto& [i, info] : file_infos) {
        pairs.emplace_back(i, &info);
    }
    return pairs;
}

void WriteDataFileHeader(const fs::path& path, uint32_t magic, uint32_t version)
{
    fs::create_directories(path.parent_path());
    AutoFile file{fsbridge::fopen(path, "wb")};
    file << magic;
    file << version;
    BOOST_REQUIRE(file.Commit());
    BOOST_REQUIRE_EQUAL(file.fclose(), 0);
}

void CorruptFirstBlockFileInfoValue(const fs::path& path)
{
    AutoFile file{fsbridge::fopen(path, "rb+")};
    file.seek(BLOCK_FILES_FILE_DATA_START_POSITION, SEEK_SET);
    uint32_t raw;
    file >> raw;
    file.seek(BLOCK_FILES_FILE_DATA_START_POSITION, SEEK_SET);
    file << (raw + 1);
    BOOST_REQUIRE_EQUAL(file.fclose(), 0);
}

uint32_t FlagChecksum(uint8_t value)
{
    return crc32c::Crc32c(&value, sizeof(value));
}

void WriteFlagValue(const fs::path& path, bool value)
{
    AutoFile file{fsbridge::fopen(path, "rb+")};
    file.seek(/*magic + version=*/8, SEEK_SET);
    const uint8_t raw_value{static_cast<uint8_t>(value ? 1 : 0)};
    file << raw_value;
    file << FlagChecksum(raw_value);
    BOOST_REQUIRE(file.Commit());
    BOOST_REQUIRE_EQUAL(file.fclose(), 0);
}

void CorruptFlagValue(const fs::path& path)
{
    AutoFile file{fsbridge::fopen(path, "rb+")};
    file.seek(/*magic + version=*/8, SEEK_SET);
    uint8_t invalid_value{2};
    file << invalid_value;
    BOOST_REQUIRE(file.Commit());
    BOOST_REQUIRE_EQUAL(file.fclose(), 0);
}

uint32_t ExtendLogChecksum(uint32_t checksum, std::span<const std::byte> value_data, int64_t position)
{
    checksum = crc32c::Extend(checksum, UCharCast(value_data.data()), value_data.size());
    std::array<std::byte, sizeof(int64_t)> position_bytes;
    WriteLE64(UCharCast(position_bytes.data()), static_cast<uint64_t>(position));
    return crc32c::Extend(checksum, UCharCast(position_bytes.data()), position_bytes.size());
}

void WriteCompleteBlockFileInfoLog(const fs::path& path, const CBlockFileInfo& info, int64_t position)
{
    AutoFile log_file{fsbridge::fopen(path, "wb")};
    log_file << LOG_FILE_MAGIC;
    log_file << LOG_FILE_VERSION;
    log_file << uint32_t{1};
    log_file << static_cast<std::underlying_type_t<ValueType>>(ValueType::BLOCK_FILE_INFO);
    log_file << uint64_t{1};

    std::array<std::byte, 36> value_buffer;
    SpanWriter{value_buffer}
        << info.nBlocks
        << info.nSize
        << info.nUndoSize
        << info.nHeightFirst
        << info.nHeightLast
        << info.nTimeFirst
        << info.nTimeLast;
    uint32_t rolling_checksum{0};
    const uint32_t checksum{ExtendLogChecksum(0, value_buffer, position)};
    rolling_checksum = ExtendLogChecksum(rolling_checksum, value_buffer, position);

    log_file.write(value_buffer);
    log_file << position;
    log_file << checksum;
    log_file << rolling_checksum;
    BOOST_REQUIRE(log_file.Commit());
    BOOST_REQUIRE_EQUAL(log_file.fclose(), 0);
}

void AppendByte(const fs::path& path)
{
    AutoFile file{fsbridge::fopen(path, "ab")};
    file << uint8_t{0};
    BOOST_REQUIRE(file.Commit());
    BOOST_REQUIRE_EQUAL(file.fclose(), 0);
}

BOOST_AUTO_TEST_CASE(HeaderFilesFormat)
{
    fs::path block_tree_store_dir{m_args.GetDataDirBase()};
    auto header_file_path{block_tree_store_dir / HEADER_FILE_NAME};
    auto block_files_file_path{block_tree_store_dir / BLOCK_FILES_FILE_NAME};
    auto log_file_path{block_tree_store_dir / LOG_FILE_NAME};
    BlockTreeStore store{block_tree_store_dir};

    AutoFile header_file{fsbridge::fopen(header_file_path, "rb")};
    uint32_t magic;
    header_file >> magic;
    BOOST_CHECK_EQUAL(magic, HEADER_FILE_MAGIC);
    uint32_t version;
    header_file >> version;
    BOOST_CHECK_EQUAL(version, HEADER_FILE_VERSION);
    header_file.seek(0, SEEK_END);
    long filesize = header_file.tell();
    BOOST_CHECK_EQUAL(filesize, HEADER_FILE_DATA_START_POSITION);
    (void)header_file.fclose();

    AutoFile block_files_file{fsbridge::fopen(block_files_file_path, "rb")};
    block_files_file >> magic;
    BOOST_CHECK_EQUAL(magic, BLOCK_FILES_FILE_MAGIC);
    block_files_file >> version;
    BOOST_CHECK_EQUAL(version, BLOCK_FILES_FILE_VERSION);
    block_files_file.seek(0, SEEK_END);
    filesize = block_files_file.tell();
    BOOST_CHECK_EQUAL(filesize, BLOCK_FILES_FILE_DATA_START_POSITION);
    (void)block_files_file.fclose();

    AutoFile log_file{fsbridge::fopen(log_file_path, "rb")};
    log_file >> magic;
    BOOST_CHECK_EQUAL(magic, LOG_FILE_MAGIC);
    log_file >> version;
    BOOST_CHECK_EQUAL(version, LOG_FILE_VERSION);
    log_file.seek(0, SEEK_END);
    filesize = log_file.tell();
    BOOST_CHECK_EQUAL(filesize, LOG_FILE_DATA_START_POSITION);
    (void)log_file.fclose();
}

BOOST_AUTO_TEST_CASE(BlockTreeStoreInvalidFiles)
{
    fs::path block_tree_store_dir{m_args.GetDataDirBase()};

    auto header_file_path{block_tree_store_dir / HEADER_FILE_NAME};
    auto block_files_file_path{block_tree_store_dir / BLOCK_FILES_FILE_NAME};

    BlockTreeStore{block_tree_store_dir};
    fs::remove(header_file_path);
    BlockTreeStore{block_tree_store_dir};
    BOOST_CHECK(fs::exists(header_file_path));

    // If both files are gone, a new store may be created
    fs::remove(header_file_path);
    fs::remove(block_files_file_path);
    BlockTreeStore{block_tree_store_dir};
    BOOST_CHECK(fs::exists(header_file_path));
    BOOST_CHECK(fs::exists(block_files_file_path));

    // If initialization was interrupted after one empty data file was
    // created, the missing empty peer may be recovered.
    fs::remove(block_files_file_path);
    BlockTreeStore{block_tree_store_dir};
    BOOST_CHECK(fs::exists(block_files_file_path));

    fs::remove(header_file_path);
    BlockTreeStore{block_tree_store_dir};
    BOOST_CHECK(fs::exists(header_file_path));

    // But a missing peer is unsafe once the existing file has records.
    {
        BlockTreeStore store{block_tree_store_dir};
        CBlockFileInfo info{CreateFileInfo(0)};
        LOCK(::cs_main);
        store.WriteBatchSync({{0, &info}}, {});
    }
    fs::remove(header_file_path);
    BOOST_CHECK_THROW(BlockTreeStore{block_tree_store_dir}, BlockTreeStoreError);
    fs::remove(block_files_file_path);

    auto write_magic_and_version{[](const fs::path& path, uint32_t magic, uint32_t version) {
        AutoFile file{fsbridge::fopen(path, "rb+")};
        file.seek(0, SEEK_SET);
        file << magic;
        file << version;
        (void)file.fclose();
    }};
    BlockTreeStore{block_tree_store_dir};
    write_magic_and_version(header_file_path, 0, 0);
    BOOST_CHECK_THROW(BlockTreeStore{block_tree_store_dir}, BlockTreeStoreError);
    write_magic_and_version(header_file_path, HEADER_FILE_MAGIC, 0);
    BOOST_CHECK_THROW(BlockTreeStore{block_tree_store_dir}, BlockTreeStoreError);
    write_magic_and_version(header_file_path, HEADER_FILE_MAGIC, HEADER_FILE_VERSION);
    BlockTreeStore{block_tree_store_dir};
}

BOOST_AUTO_TEST_CASE(BlockTreeStoreInterruptedInitialization)
{
    fs::path header_only_dir{m_args.GetDataDirBase() / "header_only"};
    WriteDataFileHeader(header_only_dir / HEADER_FILE_NAME, HEADER_FILE_MAGIC, HEADER_FILE_VERSION);
    BlockTreeStore{header_only_dir};
    BOOST_CHECK(fs::exists(header_only_dir / BLOCK_FILES_FILE_NAME));

    fs::path block_files_only_dir{m_args.GetDataDirBase() / "block_files_only"};
    WriteDataFileHeader(block_files_only_dir / BLOCK_FILES_FILE_NAME, BLOCK_FILES_FILE_MAGIC, BLOCK_FILES_FILE_VERSION);
    BlockTreeStore{block_files_only_dir};
    BOOST_CHECK(fs::exists(block_files_only_dir / HEADER_FILE_NAME));

    fs::path invalid_peer_dir{m_args.GetDataDirBase() / "invalid_peer"};
    WriteDataFileHeader(invalid_peer_dir / HEADER_FILE_NAME, 0, HEADER_FILE_VERSION);
    BOOST_CHECK_THROW(BlockTreeStore{invalid_peer_dir}, BlockTreeStoreError);

    fs::path corrupt_empty_flag_dir{m_args.GetDataDirBase() / "corrupt_empty_flag"};
    WriteDataFileHeader(corrupt_empty_flag_dir / HEADER_FILE_NAME, HEADER_FILE_MAGIC, HEADER_FILE_VERSION);
    WriteDataFileHeader(corrupt_empty_flag_dir / BLOCK_FILES_FILE_NAME, BLOCK_FILES_FILE_MAGIC, BLOCK_FILES_FILE_VERSION);
    WriteDataFileHeader(corrupt_empty_flag_dir / LOG_FLAG_FILE_NAME, 0, 0);
    BlockTreeStore{corrupt_empty_flag_dir};

    fs::path corrupt_flag_with_log_dir{m_args.GetDataDirBase() / "corrupt_flag_with_log"};
    WriteDataFileHeader(corrupt_flag_with_log_dir / HEADER_FILE_NAME, HEADER_FILE_MAGIC, HEADER_FILE_VERSION);
    WriteDataFileHeader(corrupt_flag_with_log_dir / BLOCK_FILES_FILE_NAME, BLOCK_FILES_FILE_MAGIC, BLOCK_FILES_FILE_VERSION);
    WriteDataFileHeader(corrupt_flag_with_log_dir / LOG_FLAG_FILE_NAME, 0, 0);
    WriteDataFileHeader(corrupt_flag_with_log_dir / LOG_FILE_NAME, 0, 0);
    BOOST_CHECK_THROW(BlockTreeStore{corrupt_flag_with_log_dir}, BlockTreeStoreError);

    fs::path corrupt_nonempty_flag_dir{m_args.GetDataDirBase() / "corrupt_nonempty_flag"};
    {
        BlockTreeStore store{corrupt_nonempty_flag_dir};
        CBlockFileInfo info{CreateFileInfo(0)};
        LOCK(::cs_main);
        store.WriteBatchSync({{0, &info}}, {});
    }
    WriteDataFileHeader(corrupt_nonempty_flag_dir / REINDEX_FLAG_FILE_NAME, 0, 0);
    BOOST_CHECK_THROW(BlockTreeStore{corrupt_nonempty_flag_dir}, BlockTreeStoreError);

    const auto check_corrupt_initial_flag_with_wal{[&](const char* dirname, const char* flag_name) {
        fs::path dir{m_args.GetDataDirBase() / dirname};
        {
            BlockTreeStore store{dir};
        }
        CBlockFileInfo info{CreateFileInfo(0)};
        WriteCompleteBlockFileInfoLog(dir / LOG_FILE_NAME, info, BLOCK_FILES_FILE_DATA_START_POSITION);
        WriteFlagValue(dir / LOG_FLAG_FILE_NAME, /*value=*/true);
        CorruptFlagValue(dir / flag_name);
        BOOST_CHECK_THROW(BlockTreeStore{dir}, BlockTreeStoreError);
    }};
    check_corrupt_initial_flag_with_wal("corrupt_empty_reindex_with_wal", REINDEX_FLAG_FILE_NAME);
    check_corrupt_initial_flag_with_wal("corrupt_empty_prune_with_wal", PRUNE_FLAG_FILE_NAME);

    fs::path valid_flags_truncated_log_dir{m_args.GetDataDirBase() / "valid_flags_truncated_log"};
    {
        BlockTreeStore store{valid_flags_truncated_log_dir};
        fs::resize_file(valid_flags_truncated_log_dir / LOG_FILE_NAME, 1);
        WriteFlagValue(valid_flags_truncated_log_dir / LOG_FLAG_FILE_NAME, /*value=*/false);
    }
    BlockTreeStore{valid_flags_truncated_log_dir};
}

BOOST_AUTO_TEST_CASE(BlockTreeStoreIsWriteExclusive)
{
    fs::path block_tree_store_dir{m_args.GetDataDirBase()};
    BlockTreeStore store_write{block_tree_store_dir};
    BOOST_CHECK_THROW(BlockTreeStore{block_tree_store_dir}, BlockTreeStoreError);
    BlockTreeStore store_read{block_tree_store_dir, BlockTreeStore::OpenMode::READ};
    LOCK(cs_main);
    BOOST_CHECK_THROW(store_read.WriteBatchSync({}, {}), std::logic_error);
    BOOST_CHECK_THROW(store_read.WriteReindexing(true), std::logic_error);
    BOOST_CHECK_THROW(store_read.WritePruned(true), std::logic_error);
}

BOOST_AUTO_TEST_CASE(BlockTreeStoreIncompleteWrites)
{
    LOCK(::cs_main);
    fs::path block_tree_store_dir{m_args.GetDataDirBase()};
    auto log_file{block_tree_store_dir / LOG_FILE_NAME};
    auto params{CreateChainParams(gArgs, ChainType::REGTEST)};
    auto store{std::make_unique<BlockTreeStore>(block_tree_store_dir)};

    // Write and read a CBlockFileInfo and a CBlockIndex
    ExpectedStoreState expected_state{.interrupt = m_interrupt, .params = *params};
    int32_t seed{0};
    expected_state.file_infos.insert({0, CreateFileInfo(seed)});
    auto file_infos_to_write{FileInfosToPairs(expected_state.file_infos)};
    expected_state.block_map.try_emplace(params->GenesisBlock().GetHash(), params->GenesisBlock());
    CBlockIndex* block_index = &expected_state.block_map[params->GenesisBlock().GetHash()];
    BOOST_CHECK_EQUAL(block_index->header_pos, CBlockIndex::UNSET_HEADER_POS);
    auto block_indexes_to_write{BlockMapToVector(expected_state.block_map)};

    store->SetSimulateIncompleteLogWrite(true);

    // The log file should exist in an unclean state if we abort in the middle of writing to it
    node::BlockMap block_map;
    BOOST_CHECK_THROW(store->WriteBatchSync(file_infos_to_write, block_indexes_to_write), std::runtime_error);
    BOOST_CHECK(fs::exists(log_file));
    BOOST_CHECK(store->LoadBlockIndexGuts(
        params->GetConsensus(),
        [&](const uint256& hash) { return InsertBlockIndex(block_map, hash); },
        m_interrupt));
    BOOST_CHECK(block_map.empty());

    // The constructor should ignore the log file and not apply any pending state
    block_map.clear();
    store.reset();
    store = std::make_unique<BlockTreeStore>(block_tree_store_dir);
    BOOST_CHECK(store->LoadBlockIndexGuts(
        params->GetConsensus(),
        [&](const uint256& hash) { return InsertBlockIndex(block_map, hash); },
        m_interrupt));
    BOOST_CHECK(block_map.empty());

    // Now simulate a crash in the middle of applying the log.
    block_map.clear();
    store->SetSimulateIncompleteLogApply(true);
    BOOST_CHECK_THROW(store->WriteBatchSync(file_infos_to_write, block_indexes_to_write), std::runtime_error);
    BOOST_CHECK(fs::exists(log_file));
    BOOST_CHECK_THROW(static_cast<void>(store->LoadBlockIndexGuts(
                          params->GetConsensus(),
                          [&](const uint256& hash) { return InsertBlockIndex(block_map, hash); },
                          m_interrupt)),
                      BlockTreeStoreError);
    CBlockFileInfo info;
    BOOST_CHECK_THROW(static_cast<void>(store->ReadBlockFileInfo(0, info)), BlockTreeStoreError);
    int32_t last_block;
    BOOST_CHECK_THROW(store->ReadLastBlockFile(last_block), BlockTreeStoreError);
    BlockTreeStore read_store{block_tree_store_dir, BlockTreeStore::OpenMode::READ};
    BOOST_CHECK_THROW(read_store.ReadLastBlockFile(last_block), BlockTreeStoreError);

    // The constructor should now apply the log file and remove the flag
    block_map.clear();
    store.reset();
    store = std::make_unique<BlockTreeStore>(block_tree_store_dir);
    CheckStoreContents(*store, expected_state, "constructor applies log file");
    BOOST_CHECK_EQUAL(block_index->header_pos, HEADER_FILE_DATA_START_POSITION);

    // Simulate a write application failure and subsequent write application
    store->SetSimulateIncompleteLogApply(true);
    ++seed;
    expected_state.file_infos.insert({1, CreateFileInfo(seed)});
    file_infos_to_write = FileInfosToPairs(expected_state.file_infos);
    BOOST_CHECK_THROW(store->WriteBatchSync(file_infos_to_write, block_indexes_to_write), std::runtime_error);
    store->SetSimulateIncompleteLogApply(false);
    store->WriteBatchSync({}, {});
    CheckStoreContents(*store, expected_state, "subsequent write applies log file");
}

BOOST_AUTO_TEST_CASE(BlockTreeStorePendingLogTruncation)
{
    LOCK(::cs_main);
    fs::path block_tree_store_dir{m_args.GetDataDirBase()};
    auto log_file{block_tree_store_dir / LOG_FILE_NAME};
    auto store{std::make_unique<BlockTreeStore>(block_tree_store_dir)};
    std::map<int, CBlockFileInfo> file_infos{{0, CreateFileInfo(0)}};
    auto file_infos_to_write{FileInfosToPairs(file_infos)};

    store->SetSimulateIncompleteLogApply(true);
    BOOST_CHECK_THROW(store->WriteBatchSync(file_infos_to_write, {}), std::runtime_error);
    fs::resize_file(log_file, fs::file_size(log_file) - 1);
    store.reset();
    BOOST_CHECK_THROW(BlockTreeStore{block_tree_store_dir}, BlockTreeStoreError);
}

BOOST_AUTO_TEST_CASE(BlockTreeStoreLogFlagCommitFailure)
{
    LOCK(::cs_main);
    fs::path block_tree_store_dir{m_args.GetDataDirBase()};
    auto block_files_file{block_tree_store_dir / BLOCK_FILES_FILE_NAME};
    auto header_file{block_tree_store_dir / HEADER_FILE_NAME};
    auto params{CreateChainParams(gArgs, ChainType::REGTEST)};
    auto store{std::make_unique<BlockTreeStore>(block_tree_store_dir)};
    ExpectedStoreState expected_state{.interrupt = m_interrupt, .params = *params};
    expected_state.file_infos.insert({0, CreateFileInfo(0)});

    store->SetSimulateIncompleteLogFlagCommit(true);
    BOOST_CHECK_THROW(store->WriteBatchSync(FileInfosToPairs(expected_state.file_infos), {}), std::runtime_error);
    BOOST_CHECK_EQUAL(fs::file_size(block_files_file), BLOCK_FILES_FILE_DATA_START_POSITION);
    BOOST_CHECK_EQUAL(fs::file_size(header_file), HEADER_FILE_DATA_START_POSITION);

    BOOST_CHECK_THROW(store->WriteBatchSync({}, {}), std::runtime_error);
    BOOST_CHECK_EQUAL(fs::file_size(block_files_file), BLOCK_FILES_FILE_DATA_START_POSITION);
    BOOST_CHECK_EQUAL(fs::file_size(header_file), HEADER_FILE_DATA_START_POSITION);

    store->SetSimulateIncompleteLogFlagCommit(false);
    store->WriteBatchSync({}, {});
    CheckStoreContents(*store, expected_state, "retry applies log after log flag commit succeeds");
}

BOOST_AUTO_TEST_CASE(BlockTreeStorePendingLogRepairsPartialAppend)
{
    LOCK(::cs_main);
    fs::path block_tree_store_dir{m_args.GetDataDirBase()};
    auto block_files_file{block_tree_store_dir / BLOCK_FILES_FILE_NAME};
    auto params{CreateChainParams(gArgs, ChainType::REGTEST)};
    auto store{std::make_unique<BlockTreeStore>(block_tree_store_dir)};
    ExpectedStoreState expected_state{.interrupt = m_interrupt, .params = *params};
    expected_state.file_infos.insert({0, CreateFileInfo(0)});

    store->SetSimulateIncompleteLogApply(true);
    BOOST_CHECK_THROW(store->WriteBatchSync(FileInfosToPairs(expected_state.file_infos), {}), std::runtime_error);
    fs::resize_file(block_files_file, BLOCK_FILES_FILE_DATA_START_POSITION + 1);

    BlockTreeStore read_store{block_tree_store_dir, BlockTreeStore::OpenMode::READ};
    int32_t last_block;
    BOOST_CHECK_THROW(read_store.ReadLastBlockFile(last_block), BlockTreeStoreError);

    store.reset();
    store = std::make_unique<BlockTreeStore>(block_tree_store_dir);
    CheckStoreContents(*store, expected_state, "pending WAL repairs partial block file info append");
}

BOOST_AUTO_TEST_CASE(BlockTreeStoreWALFlagRecovery)
{
    fs::path block_tree_store_dir{m_args.GetDataDirBase()};
    auto params{CreateChainParams(gArgs, ChainType::REGTEST)};

    const auto check_read_only_rejects{[](const fs::path& dir) {
        BlockTreeStore read_store{dir, BlockTreeStore::OpenMode::READ};
        int32_t last_block;
        BOOST_CHECK_THROW(read_store.ReadLastBlockFile(last_block), BlockTreeStoreError);
    }};

    const auto check_recovery_after_completed_write{[&](const fs::path& dir, bool remove_flag) {
        auto store{std::make_unique<BlockTreeStore>(dir)};
        ExpectedStoreState expected_state{.interrupt = m_interrupt, .params = *params};
        expected_state.file_infos.insert({0, CreateFileInfo(0)});
        {
            LOCK(::cs_main);
            store->WriteBatchSync(FileInfosToPairs(expected_state.file_infos), {});
        }

        if (remove_flag) {
            BOOST_REQUIRE(fs::remove(dir / LOG_FLAG_FILE_NAME));
        } else {
            CorruptFlagValue(dir / LOG_FLAG_FILE_NAME);
        }
        check_read_only_rejects(dir);

        store.reset();
        store = std::make_unique<BlockTreeStore>(dir);
        CheckStoreContents(*store, expected_state, remove_flag ? "missing WAL flag recovered" : "corrupt WAL flag recovered");
    }};

    check_recovery_after_completed_write(block_tree_store_dir / "completed_corrupt", /*remove_flag=*/false);
    check_recovery_after_completed_write(block_tree_store_dir / "completed_missing", /*remove_flag=*/true);

    fs::path interrupted_dir{block_tree_store_dir / "interrupted_apply"};
    auto store{std::make_unique<BlockTreeStore>(interrupted_dir)};
    ExpectedStoreState expected_state{.interrupt = m_interrupt, .params = *params};
    expected_state.file_infos.insert({0, CreateFileInfo(1)});
    store->SetSimulateIncompleteLogApply(true);
    {
        LOCK(::cs_main);
        BOOST_CHECK_THROW(store->WriteBatchSync(FileInfosToPairs(expected_state.file_infos), {}), std::runtime_error);
    }
    CorruptFlagValue(interrupted_dir / LOG_FLAG_FILE_NAME);
    check_read_only_rejects(interrupted_dir);

    store.reset();
    store = std::make_unique<BlockTreeStore>(interrupted_dir);
    CheckStoreContents(*store, expected_state, "corrupt WAL flag recovered after interrupted apply");
}

BOOST_AUTO_TEST_CASE(BlockTreeStoreInvalidWALFlagIncompleteLog)
{
    LOCK(::cs_main);
    fs::path block_tree_store_dir{m_args.GetDataDirBase()};
    auto store{std::make_unique<BlockTreeStore>(block_tree_store_dir)};
    std::map<int, CBlockFileInfo> file_infos{{0, CreateFileInfo(0)}};

    store->SetSimulateIncompleteLogWrite(true);
    BOOST_CHECK_THROW(store->WriteBatchSync(FileInfosToPairs(file_infos), {}), std::runtime_error);
    CorruptFlagValue(block_tree_store_dir / LOG_FLAG_FILE_NAME);
    store.reset();
    BOOST_CHECK_THROW(BlockTreeStore{block_tree_store_dir}, BlockTreeStoreError);
}

BOOST_AUTO_TEST_CASE(BlockTreeStoreWALBounds)
{
    LOCK(::cs_main);
    fs::path block_tree_store_dir{m_args.GetDataDirBase()};
    CBlockFileInfo info{CreateFileInfo(0)};

    {
        auto store{std::make_unique<BlockTreeStore>(block_tree_store_dir / "negative_file")};
        BOOST_CHECK_THROW(store->WriteBatchSync({{-1, &info}}, {}), BlockTreeStoreError);
    }

    fs::path header_target_dir{block_tree_store_dir / "header_target"};
    {
        auto store{std::make_unique<BlockTreeStore>(header_target_dir)};
        WriteCompleteBlockFileInfoLog(header_target_dir / LOG_FILE_NAME, info, /*position=*/0);
        WriteFlagValue(header_target_dir / LOG_FLAG_FILE_NAME, /*value=*/true);
        store.reset();
    }
    BOOST_CHECK_THROW(BlockTreeStore{header_target_dir}, BlockTreeStoreError);

    AutoFile block_files_file{fsbridge::fopen(header_target_dir / BLOCK_FILES_FILE_NAME, "rb")};
    uint32_t magic;
    block_files_file >> magic;
    BOOST_CHECK_EQUAL(magic, BLOCK_FILES_FILE_MAGIC);
    BOOST_REQUIRE_EQUAL(block_files_file.fclose(), 0);

    fs::path sparse_target_dir{block_tree_store_dir / "sparse_target"};
    {
        auto store{std::make_unique<BlockTreeStore>(sparse_target_dir)};
        WriteCompleteBlockFileInfoLog(sparse_target_dir / LOG_FILE_NAME, info, BLOCK_FILES_FILE_DATA_START_POSITION + 2 * BLOCK_FILE_INFO_RECORD_SIZE);
        WriteFlagValue(sparse_target_dir / LOG_FLAG_FILE_NAME, /*value=*/true);
        store.reset();
    }
    BOOST_CHECK_THROW(BlockTreeStore{sparse_target_dir}, BlockTreeStoreError);

    fs::path trailing_log_dir{block_tree_store_dir / "trailing_log"};
    {
        auto store{std::make_unique<BlockTreeStore>(trailing_log_dir)};
        WriteCompleteBlockFileInfoLog(trailing_log_dir / LOG_FILE_NAME, info, BLOCK_FILES_FILE_DATA_START_POSITION);
        AppendByte(trailing_log_dir / LOG_FILE_NAME);
        WriteFlagValue(trailing_log_dir / LOG_FLAG_FILE_NAME, /*value=*/true);
        store.reset();
    }
    BOOST_CHECK_THROW(BlockTreeStore{trailing_log_dir}, BlockTreeStoreError);
}

BOOST_AUTO_TEST_CASE(BlockTreeStoreFlags)
{
    auto store{std::make_unique<BlockTreeStore>(m_args.GetDataDirBase())};
    bool reindexing = true;
    store->ReadReindexing(reindexing);
    BOOST_CHECK(!reindexing);
    store->WriteReindexing(true);
    store->ReadReindexing(reindexing);
    BOOST_CHECK(reindexing);
    store->WriteReindexing(false);
    store->ReadReindexing(reindexing);
    BOOST_CHECK(!reindexing);

    int last_block;
    store->ReadLastBlockFile(last_block);
    BOOST_CHECK_EQUAL(last_block, 0);

    bool pruned = false;
    store->ReadPruned(pruned);
    BOOST_CHECK(!pruned);
    store->WritePruned(true);
    store->ReadPruned(pruned);
    BOOST_CHECK(pruned);
    store->WritePruned(false);
    store->ReadPruned(pruned);
    BOOST_CHECK(!pruned);

    // Re-create the store and check that the data was persisted
    store->WritePruned(true);
    store->WriteReindexing(true);
    store.reset();
    store = std::make_unique<BlockTreeStore>(m_args.GetDataDirBase());
    store->ReadPruned(pruned);
    store->ReadReindexing(reindexing);
    BOOST_CHECK(pruned);
    BOOST_CHECK(reindexing);
}

BOOST_AUTO_TEST_CASE(BlockTreeStoreFlagIntegrity)
{
    fs::path block_tree_store_dir{m_args.GetDataDirBase()};
    auto store{std::make_unique<BlockTreeStore>(block_tree_store_dir)};
    CBlockFileInfo info{CreateFileInfo(0)};
    {
        LOCK(::cs_main);
        store->WriteBatchSync({{0, &info}}, {});
    }
    bool reindexing{false};
    store->WriteReindexing(true);
    store->ReadReindexing(reindexing);
    BOOST_CHECK(reindexing);

    CorruptFlagValue(block_tree_store_dir / REINDEX_FLAG_FILE_NAME);
    BOOST_CHECK_THROW(store->ReadReindexing(reindexing), BlockTreeStoreError);
    store.reset();
    BOOST_CHECK_THROW(BlockTreeStore{block_tree_store_dir}, BlockTreeStoreError);
}

BOOST_AUTO_TEST_CASE(BlockTreeStoreInterruptedFlagReplacement)
{
    fs::path block_tree_store_dir{m_args.GetDataDirBase()};
    auto store{std::make_unique<BlockTreeStore>(block_tree_store_dir)};
    CBlockFileInfo info{CreateFileInfo(0)};
    {
        LOCK(::cs_main);
        store->WriteBatchSync({{0, &info}}, {});
    }
    store->WriteReindexing(true);

    fs::path reindex_tmp{block_tree_store_dir / REINDEX_FLAG_FILE_NAME};
    reindex_tmp += ".tmp";
    fs::path prune_tmp{block_tree_store_dir / PRUNE_FLAG_FILE_NAME};
    prune_tmp += ".tmp";

    store->SetSimulateIncompleteFlagWrite(true);
    BOOST_CHECK_THROW(store->WriteReindexing(false), std::runtime_error);
    BOOST_CHECK_THROW(store->WritePruned(true), std::runtime_error);
    BOOST_CHECK(fs::exists(reindex_tmp));
    BOOST_CHECK(fs::exists(prune_tmp));

    store.reset();
    store = std::make_unique<BlockTreeStore>(block_tree_store_dir);
    bool reindexing{false};
    store->ReadReindexing(reindexing);
    BOOST_CHECK(reindexing);
    bool pruned{false};
    store->ReadPruned(pruned);
    BOOST_CHECK(!pruned);

    store->WriteReindexing(false);
    store->WritePruned(true);
    store->ReadReindexing(reindexing);
    BOOST_CHECK(!reindexing);
    store->ReadPruned(pruned);
    BOOST_CHECK(pruned);
    BOOST_CHECK(!fs::exists(reindex_tmp));
    BOOST_CHECK(!fs::exists(prune_tmp));
}

BOOST_AUTO_TEST_CASE(BlockTreeStoreReadBlockFileInfoIntegrity)
{
    fs::path block_tree_store_dir{m_args.GetDataDirBase()};
    auto block_files_file_path{block_tree_store_dir / BLOCK_FILES_FILE_NAME};
    CBlockFileInfo info{CreateFileInfo(0)};
    {
        BlockTreeStore store{block_tree_store_dir};
        CBlockFileInfo retrieved_info;
        BOOST_CHECK(!store.ReadBlockFileInfo(-1, retrieved_info));
        BOOST_CHECK(!store.ReadBlockFileInfo(0, retrieved_info));
        LOCK(::cs_main);
        store.WriteBatchSync({{0, &info}}, {});
        BOOST_CHECK(store.ReadBlockFileInfo(0, retrieved_info));
        BOOST_CHECK(!store.ReadBlockFileInfo(1, retrieved_info));
    }
    CorruptFirstBlockFileInfoValue(block_files_file_path);
    {
        BlockTreeStore store{block_tree_store_dir, BlockTreeStore::OpenMode::READ};
        CBlockFileInfo retrieved_info;
        BOOST_CHECK_THROW(static_cast<void>(store.ReadBlockFileInfo(0, retrieved_info)), BlockTreeStoreError);
    }

    fs::path truncated_dir{m_args.GetDataDirBase() / "truncated"};
    auto truncated_block_files_file_path{truncated_dir / BLOCK_FILES_FILE_NAME};
    {
        BlockTreeStore store{truncated_dir};
        LOCK(::cs_main);
        store.WriteBatchSync({{0, &info}}, {});
    }
    fs::resize_file(truncated_block_files_file_path, fs::file_size(truncated_block_files_file_path) - 1);
    BlockTreeStore store{truncated_dir, BlockTreeStore::OpenMode::READ};
    CBlockFileInfo retrieved_info;
    BOOST_CHECK_THROW(static_cast<void>(store.ReadBlockFileInfo(0, retrieved_info)), BlockTreeStoreError);
}

CBlockIndex* AddTestBlockIndex(node::BlockMap& test_block_map, const CBlockHeader& header, CBlockIndex* prev)
{
    LOCK(::cs_main);
    const auto [mi, inserted]{test_block_map.try_emplace(header.GetHash(), header)};
    CBlockIndex* pindex{&mi->second};
    pindex->phashBlock = &mi->first;
    pindex->pprev = prev;
    pindex->nHeight = prev ? prev->nHeight + 1 : 0;
    pindex->nStatus = prev ? prev->nStatus ^ BLOCK_FAILED_VALID : 0;
    pindex->nTx = prev ? prev->nTx + 3 : 0;
    pindex->nFile = prev ? prev->nFile + 4 : 0;
    pindex->nDataPos = prev ? prev->nDataPos + 100 : 0;
    pindex->nUndoPos = prev ? prev->nUndoPos + 101 : 0;
    return pindex;
}

void WriteAndCheckBlockIndex(BlockTreeStore& store,
                             ExpectedStoreState& expected_state,
                             node::BlockMap& block_map_to_write,
                             std::span<CBlockFileInfo> file_infos_to_write,
                             const std::string& context)
{
    LOCK(::cs_main);
    const auto block_indexes_to_write{BlockMapToVector(block_map_to_write)};
    const auto file_info_pairs_to_write{FileInfosToPairs(file_infos_to_write)};
    store.WriteBatchSync(file_info_pairs_to_write, block_indexes_to_write);
    CheckStoreContents(store, expected_state, context);
}

BOOST_AUTO_TEST_CASE(BlockTreeStoreRW)
{
    LOCK(::cs_main);
    fs::path block_tree_store_dir{m_args.GetDataDirBase()};
    auto header_file{block_tree_store_dir / HEADER_FILE_NAME};
    auto block_files_file{block_tree_store_dir / BLOCK_FILES_FILE_NAME};
    auto params{CreateChainParams(gArgs, ChainType::REGTEST)};
    BlockTreeStore store{block_tree_store_dir};

    node::BlockMap block_map_to_write;
    std::vector<CBlockFileInfo> file_info_to_write;
    ExpectedStoreState expected_state{.interrupt = m_interrupt, .params = *params};

    // Check that the store is empty
    CheckStoreContents(store, expected_state, "check empty store");

    // Write and read a CBlockFileInfo and a CBlockIndex
    int32_t counter = 0;
    file_info_to_write.emplace_back(CreateFileInfo(counter));
    expected_state.file_infos.emplace(counter, CreateFileInfo(counter));
    CBlockIndex* block_index = AddTestBlockIndex(block_map_to_write, params->GenesisBlock(), /*prev=*/nullptr);
    AddTestBlockIndex(expected_state.block_map, params->GenesisBlock(), /*prev*/ nullptr);
    BOOST_CHECK_EQUAL(block_index->header_pos, CBlockIndex::UNSET_HEADER_POS);
    WriteAndCheckBlockIndex(store, expected_state, block_map_to_write, file_info_to_write, "write and read single entries");
    BOOST_CHECK_EQUAL(block_index->header_pos, HEADER_FILE_DATA_START_POSITION);

    // Write another CBlockFileInfo and update the CBlockIndex
    ++counter;
    file_info_to_write.emplace_back(CreateFileInfo(counter));
    expected_state.file_infos.emplace(counter, CreateFileInfo(counter));
    block_index->nStatus = 120;
    expected_state.block_map[block_index->GetBlockHash()].nStatus = 120;
    block_index->nFile = 1;
    expected_state.block_map[block_index->GetBlockHash()].nFile = 1;
    WriteAndCheckBlockIndex(store, expected_state, block_map_to_write, file_info_to_write, "write another file info and update CBlockIndex");

    // Write an empty block map
    node::BlockMap empty_block_map{};
    WriteAndCheckBlockIndex(store, expected_state, empty_block_map, file_info_to_write, "write empty block map");

    // Write an empty file info vector
    std::vector<CBlockFileInfo> empty_file_info;
    WriteAndCheckBlockIndex(store, expected_state, block_map_to_write, empty_file_info, "write empty file info");

    // Update the new CBlockFileInfo and the CBlockIndex and check that the file sizes are unchanged
    block_index->nStatus = 99;
    expected_state.block_map[block_index->GetBlockHash()].nStatus = 99;
    block_index->nFile = 50;
    expected_state.block_map[block_index->GetBlockHash()].nFile = 50;
    file_info_to_write[1] = CreateFileInfo(3);
    expected_state.file_infos[1] = CreateFileInfo(3);
    auto header_file_size{fs::file_size(header_file)};
    auto block_files_file_size{fs::file_size(block_files_file)};
    WriteAndCheckBlockIndex(store, expected_state, block_map_to_write, file_info_to_write, "update existing entries");
    BOOST_CHECK_EQUAL(header_file_size, fs::file_size(header_file));
    BOOST_CHECK_EQUAL(block_files_file_size, fs::file_size(block_files_file));

    // Add more CBlockIndex entries to the store
    BOOST_CHECK_EQUAL(expected_state.block_map.size(), 1);
    for (uint8_t i = 0; i < 10; ++i) {
        CBlockHeader header;
        header.hashPrevBlock = block_index->GetBlockHash();
        header.nBits = params->GenesisBlock().nBits;
        header.nTime = block_index->nTime + 1;
        header.hashMerkleRoot = uint256{i};
        while (!CheckProofOfWork(header.GetHash(), header.nBits, params->GetConsensus())) {
            ++header.nNonce;
        }
        block_index = AddTestBlockIndex(expected_state.block_map, header, /*prev=*/block_index);
        // Add a couple of forks too
        if (i % 3 == 0) {
            block_index = block_index->pprev;
        }
    }
    BOOST_CHECK_EQUAL(expected_state.block_map.size(), 11);
    WriteAndCheckBlockIndex(store, expected_state, expected_state.block_map, file_info_to_write, "add a tree of block indexes");

    // Read and write back the same data and check that the file sizes are unchanged
    header_file_size = fs::file_size(header_file);
    node::BlockMap loaded_block_map;
    BOOST_CHECK(store.LoadBlockIndexGuts(
        params->GetConsensus(),
        [&](const uint256& hash) { return InsertBlockIndex(loaded_block_map, hash); },
        m_interrupt));
    WriteAndCheckBlockIndex(store, expected_state, loaded_block_map, file_info_to_write, "read and write the same data leaves no changes");
    BOOST_CHECK_EQUAL(header_file_size, fs::file_size(header_file));

    // Writing an invalid CBlockIndex (with invalid PoW) should fail to load
    block_map_to_write.begin()->second.nBits = 0;
    loaded_block_map.clear();
    store.WriteBatchSync(FileInfosToPairs(file_info_to_write), BlockMapToVector(block_map_to_write));
    BOOST_CHECK(!store.LoadBlockIndexGuts(
        params->GetConsensus(),
        [&](const uint256& hash) { return InsertBlockIndex(loaded_block_map, hash); },
        m_interrupt));
}

BOOST_AUTO_TEST_SUITE_END()
