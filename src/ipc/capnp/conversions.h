// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef BITCOIN_IPC_CAPNP_CONVERSIONS_H
#define BITCOIN_IPC_CAPNP_CONVERSIONS_H

#include <ipc/capnp/common.capnp.h>
#include <ipc/capnp/mining.capnp.h>
#include <node/types.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <serialize.h>
#include <span.h>
#include <streams.h>
#include <uint256.h>
#include <univalue.h>

#include <capnp/blob.h>

#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace interfaces {
struct BlockRef;
} // namespace interfaces

namespace ipc {
namespace capnp {

std::span<const unsigned char> DataSpan(::capnp::Data::Reader data);
::capnp::Data::Reader MakeDataReader(std::span<const unsigned char> bytes);
void CopyData(::capnp::Data::Builder output, std::span<const unsigned char> bytes);

template <typename Stream>
auto WrapSerialization(Stream& stream)
{
    return ParamsStream{stream, TX_WITH_WITNESS};
}

template <typename LocalType>
std::vector<unsigned char> SerializeData(const LocalType& value)
{
    DataStream stream;
    auto wrapped{WrapSerialization(stream)};
    value.Serialize(wrapped);

    std::vector<unsigned char> bytes(stream.size());
    if (!bytes.empty()) std::memcpy(bytes.data(), stream.data(), stream.size());
    return bytes;
}

template <typename LocalType>
LocalType ReadData(::capnp::Data::Reader input)
{
    SpanReader stream{DataSpan(input)};
    auto wrapped{WrapSerialization(stream)};
    LocalType value;
    value.Unserialize(wrapped);
    return value;
}

uint256 ReadUint256(::capnp::Data::Reader input);
void BuildUint256(::capnp::Data::Builder output, const uint256& value);

CTransactionRef ReadTransaction(::capnp::Data::Reader input);

interfaces::BlockRef ReadBlockRef(messages::BlockRef::Reader input);
void BuildBlockRef(messages::BlockRef::Builder output, const interfaces::BlockRef& value);

node::BlockCreateOptions ReadBlockCreateOptions(messages::BlockCreateOptions::Reader input);
void BuildBlockCreateOptions(messages::BlockCreateOptions::Builder output, const node::BlockCreateOptions& value);

node::BlockWaitOptions ReadBlockWaitOptions(messages::BlockWaitOptions::Reader input);
void BuildBlockWaitOptions(messages::BlockWaitOptions::Builder output, const node::BlockWaitOptions& value);

node::BlockCheckOptions ReadBlockCheckOptions(messages::BlockCheckOptions::Reader input);
void BuildBlockCheckOptions(messages::BlockCheckOptions::Builder output, const node::BlockCheckOptions& value);

node::CoinbaseTx ReadCoinbaseTx(messages::CoinbaseTx::Reader input);
void BuildCoinbaseTx(messages::CoinbaseTx::Builder output, const node::CoinbaseTx& value);

UniValue ReadUniValue(std::string_view json);
std::string WriteUniValue(const UniValue& value);

} // namespace capnp
} // namespace ipc

#endif // BITCOIN_IPC_CAPNP_CONVERSIONS_H
