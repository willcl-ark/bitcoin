// Copyright (c) 2026 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <kernel/bitcoinkernel.h>
#include <kernel/bitcoinkernel_wrapper.h>

#include <array>
#include <charconv>
#include <cstddef>
#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

using namespace btck;

namespace {
void Check(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error{message};
}

template <typename T>
T ParseInteger(std::string_view text)
{
    T value{};
    auto [end, error]{std::from_chars(text.data(), text.data() + text.size(), value)};
    Check(error == std::errc{} && end == text.data() + text.size(), "Invalid integer");
    return value;
}

BlockHash ParseHash(std::string_view hex)
{
    Check(hex.size() == 64, "Invalid hash length");
    std::array<std::byte, 32> bytes;
    for (size_t i{0}; i < bytes.size(); ++i) {
        unsigned int byte;
        auto [end, error]{std::from_chars(hex.data() + 2 * i, hex.data() + 2 * i + 2, byte, 16)};
        Check(error == std::errc{} && end == hex.data() + 2 * i + 2, "Invalid hash");
        bytes[31 - i] = static_cast<std::byte>(byte);
    }
    return BlockHash{bytes};
}

class KernelLog
{
public:
    void LogMessage(std::string_view message) { std::cerr << message; }
};
} // namespace

// Kept in a compiled process so the reader uses the build's architecture,
// sanitizer runtime, and Windows UTF-8 manifest, rather than Python's.
int main(int argc, char* argv[])
{
    try {
        Check(argc == 7, "Expected datadir, block hash, height, transaction count, genesis hash, and tip height");
        Logger logger{std::make_unique<KernelLog>()};
        ContextOptions options;
        ChainParams params{ChainType::REGTEST};
        options.SetChainParams(params);
        Context context{options};
        const std::string datadir{argv[1]};
        const std::string blocksdir{datadir + "/blocks"};
        BlockTreeReader reader{context, datadir, blocksdir};
        const size_t snapshot_count{reader.CountEntries()};
        const auto genesis{reader.GetEntry(0)};
        Check(genesis.GetHeight() == 0, "Incorrect genesis height");
        Check(genesis.GetHash().ToBytes() == ParseHash(argv[5]).ToBytes(), "Incorrect genesis hash");
        Check(!btck_block_tree_reader_get_entry_at(reader.get(), snapshot_count), "Out-of-range entry exists");
        const auto hash{ParseHash(argv[2])};
        const auto entry{reader.GetBlockTreeEntry(hash)};
        Check(entry.has_value(), "Block entry missing");
        Check(entry->GetHeight() == ParseInteger<int>(argv[3]), "Incorrect block height");
        Check(entry->GetHash().ToBytes() == hash.ToBytes(), "Incorrect entry hash");
        Check(snapshot_count >= ParseInteger<size_t>(argv[6]) + 1, "Too few entries");
        const auto block{reader.ReadBlock(*entry)};
        Check(block.has_value(), "Block read failed");
        const size_t transactions{ParseInteger<size_t>(argv[4])};
        Check(block->CountTransactions() == transactions, "Incorrect transaction count");
        Check(block->GetHash().ToBytes() == hash.ToBytes(), "Incorrect block hash");
        Check(reader.ReadBlockSpentOutputs(*entry).Count() == transactions - 1, "Incorrect spent outputs count");
        std::cout << "ready" << std::endl;

        // Python submits and flushes a header while this snapshot stays open.
        std::string header_hex;
        int header_height;
        Check(static_cast<bool>(std::cin >> header_hex >> header_height), "Expected new header hash and height");
        const auto header_hash{ParseHash(header_hex)};
        Check(reader.CountEntries() == snapshot_count, "Snapshot changed in place");
        Check(!reader.GetBlockTreeEntry(header_hash), "Snapshot contains the new header");
        BlockTreeReader refreshed{context, datadir, blocksdir};
        Check(refreshed.CountEntries() >= snapshot_count + 1, "Refreshed reader lacks new entry");
        const auto header{refreshed.GetBlockTreeEntry(header_hash)};
        Check(header.has_value(), "New header missing");
        Check(header->GetHeight() == header_height, "Incorrect header height");
        Check(header->GetHash().ToBytes() == header_hash.ToBytes(), "Incorrect header hash");
        std::cout << "ok" << std::endl;
    } catch (const std::exception& error) {
        std::cerr << error.what() << std::endl;
        return 1;
    }
}
