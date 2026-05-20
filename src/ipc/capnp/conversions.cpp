// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <ipc/capnp/conversions.h>

#include <interfaces/types.h>

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <memory>
#include <optional>

namespace ipc {
namespace capnp {

std::span<const unsigned char> DataSpan(::capnp::Data::Reader data)
{
    return {reinterpret_cast<const unsigned char*>(data.begin()), data.size()};
}

::capnp::Data::Reader MakeDataReader(std::span<const unsigned char> bytes)
{
    return {reinterpret_cast<const ::capnp::byte*>(bytes.data()), bytes.size()};
}

void CopyData(::capnp::Data::Builder output, std::span<const unsigned char> bytes)
{
    assert(output.size() == bytes.size());
    std::copy(bytes.begin(), bytes.end(), output.begin());
}

uint256 ReadUint256(::capnp::Data::Reader input)
{
    return uint256{DataSpan(input)};
}

void BuildUint256(::capnp::Data::Builder output, const uint256& value)
{
    CopyData(output, {value.data(), value.size()});
}

CTransactionRef ReadTransaction(::capnp::Data::Reader input)
{
    SpanReader stream{DataSpan(input)};
    auto wrapped{WrapSerialization(stream)};
    return std::make_shared<const CTransaction>(deserialize, wrapped);
}

interfaces::BlockRef ReadBlockRef(messages::BlockRef::Reader input)
{
    return {ReadUint256(input.getHash()), input.getHeight()};
}

void BuildBlockRef(messages::BlockRef::Builder output, const interfaces::BlockRef& value)
{
    output.setHash(MakeDataReader({value.hash.data(), value.hash.size()}));
    output.setHeight(value.height);
}

std::optional<interfaces::BlockRef> ReadOptionalBlockRef(messages::OptionalBlockRef::Reader input)
{
    switch (input.which()) {
    case messages::OptionalBlockRef::NONE:
        return std::nullopt;
    case messages::OptionalBlockRef::VALUE:
        return ReadBlockRef(input.getValue());
    }
    assert(false);
    return std::nullopt;
}

void BuildOptionalBlockRef(messages::OptionalBlockRef::Builder output, const std::optional<interfaces::BlockRef>& value)
{
    if (value) {
        BuildBlockRef(output.initValue(), *value);
    } else {
        output.setNone({});
    }
}

node::BlockCreateOptions ReadBlockCreateOptions(messages::BlockCreateOptions::Reader input)
{
    node::BlockCreateOptions options;
    options.use_mempool = input.getUseMempool();
    options.block_reserved_weight = input.getBlockReservedWeight();
    options.coinbase_output_max_additional_sigops = input.getCoinbaseOutputMaxAdditionalSigops();
    return options;
}

void BuildBlockCreateOptions(messages::BlockCreateOptions::Builder output, const node::BlockCreateOptions& value)
{
    output.setUseMempool(value.use_mempool);
    if (value.block_reserved_weight) output.setBlockReservedWeight(*value.block_reserved_weight);
    output.setCoinbaseOutputMaxAdditionalSigops(value.coinbase_output_max_additional_sigops);
}

node::BlockWaitOptions ReadBlockWaitOptions(messages::BlockWaitOptions::Reader input)
{
    node::BlockWaitOptions options;
    options.timeout = MillisecondsDouble{input.getTimeout()};
    options.fee_threshold = input.getFeeThreshold();
    return options;
}

void BuildBlockWaitOptions(messages::BlockWaitOptions::Builder output, const node::BlockWaitOptions& value)
{
    output.setTimeout(value.timeout.count());
    output.setFeeThreshold(value.fee_threshold);
}

node::BlockCheckOptions ReadBlockCheckOptions(messages::BlockCheckOptions::Reader input)
{
    node::BlockCheckOptions options;
    options.check_merkle_root = input.getCheckMerkleRoot();
    options.check_pow = input.getCheckPow();
    return options;
}

void BuildBlockCheckOptions(messages::BlockCheckOptions::Builder output, const node::BlockCheckOptions& value)
{
    output.setCheckMerkleRoot(value.check_merkle_root);
    output.setCheckPow(value.check_pow);
}

node::CoinbaseTx ReadCoinbaseTx(messages::CoinbaseTx::Reader input)
{
    node::CoinbaseTx tx;
    tx.version = input.getVersion();
    tx.sequence = input.getSequence();

    auto script_sig_prefix{DataSpan(input.getScriptSigPrefix())};
    tx.script_sig_prefix = CScript{script_sig_prefix.begin(), script_sig_prefix.end()};

    if (input.hasWitness()) tx.witness = ReadUint256(input.getWitness());
    tx.block_reward_remaining = input.getBlockRewardRemaining();

    for (auto output : input.getRequiredOutputs()) {
        tx.required_outputs.push_back(ReadData<CTxOut>(output));
    }
    tx.lock_time = input.getLockTime();
    return tx;
}

void BuildCoinbaseTx(messages::CoinbaseTx::Builder output, const node::CoinbaseTx& value)
{
    output.setVersion(value.version);
    output.setSequence(value.sequence);
    output.setScriptSigPrefix(MakeDataReader({value.script_sig_prefix.data(), value.script_sig_prefix.size()}));
    if (value.witness) output.setWitness(MakeDataReader({value.witness->data(), value.witness->size()}));
    output.setBlockRewardRemaining(value.block_reward_remaining);

    std::vector<std::vector<unsigned char>> serialized_outputs;
    serialized_outputs.reserve(value.required_outputs.size());
    std::vector<::capnp::Data::Reader> output_readers;
    output_readers.reserve(value.required_outputs.size());
    for (const auto& required_output : value.required_outputs) {
        serialized_outputs.push_back(SerializeData(required_output));
        output_readers.push_back(MakeDataReader(serialized_outputs.back()));
    }
    output.setRequiredOutputs({output_readers.data(), output_readers.size()});
    output.setLockTime(value.lock_time);
}

UniValue ReadUniValue(std::string_view json)
{
    UniValue value;
    value.read(json);
    return value;
}

std::string WriteUniValue(const UniValue& value)
{
    return value.write();
}

} // namespace capnp
} // namespace ipc
