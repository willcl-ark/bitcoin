// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_NODE_MESSAGE_TIP_RULE_H
#define BITCOIN_NODE_MESSAGE_TIP_RULE_H

#include <protocol.h>

#include <cstdint>
#include <optional>
#include <string_view>

namespace node {
/**
 * What a peer's message needs from blocks that the message handler accepted
 * before it: because the message handler hands block activation off to the
 * connector thread, a subsequent message may arrive while the active tip lags
 * behind a previously received block.
 *
 * Every known message type has a row in MESSAGE_TIP_RULES.
 * PeerManagerImpl::ProcessMessage() consults this table once before running a
 * handler and waits there (PeerManagerImpl::WaitForPendingBlocks()) when out
 * of initial block download.
 *
 * Two non-message entry points also depend on the tip: orphan reconsideration
 * (PeerManagerImpl::ProcessOrphanTx()), which waits directly, and deferred
 * GETDATA block requests (PeerManagerImpl::ProcessGetBlockData()), which wait
 * for storage or activate the chain themselves when out of IBD.
 */
enum class TipRule : uint8_t {
    //! The handler reads nothing that a block moves.
    Nothing,
    //! The handler tolerates a lagging tip, or synchronizes by its own mechanism.
    TipLagTolerated,
    //! Pending blocks must be connected before this handler runs (outside IBD).
    PendingBlocksConnected,
};

struct MessageTipRule {
    std::string_view type;
    TipRule rule;
};

// clang-format off
inline constexpr MessageTipRule MESSAGE_TIP_RULES[]{
    {NetMsgType::VERSION,      TipRule::Nothing},                // handshake; height is the peer's
    {NetMsgType::VERACK,       TipRule::Nothing},                // handshake
    {NetMsgType::ADDR,         TipRule::Nothing},                // addresses
    {NetMsgType::ADDRV2,       TipRule::Nothing},                // addresses
    {NetMsgType::SENDADDRV2,   TipRule::Nothing},                // peer setting
    {NetMsgType::INV,          TipRule::PendingBlocksConnected}, // recent-rejects filter resets when the tip changes
    {NetMsgType::GETDATA,      TipRule::TipLagTolerated},        // ProcessGetBlockData() waits for storage or activates the chain
    {NetMsgType::MERKLEBLOCK,  TipRule::Nothing},                // not processed
    {NetMsgType::GETBLOCKS,    TipRule::TipLagTolerated},        // activates the chain itself before answering
    {NetMsgType::GETHEADERS,   TipRule::PendingBlocksConnected}, // answered from the active chain
    {NetMsgType::TX,           TipRule::PendingBlocksConnected}, // validated against tip coins; may spend outputs from a prior block
    {NetMsgType::HEADERS,      TipRule::TipLagTolerated},        // headers update the block index directly
    {NetMsgType::BLOCK,        TipRule::PendingBlocksConnected}, // sees prior block connected and announces NewPoWValidBlock cleanly
    {NetMsgType::GETADDR,      TipRule::Nothing},                // addresses
    {NetMsgType::MEMPOOL,      TipRule::TipLagTolerated},        // answered from current mempool
    {NetMsgType::PING,         TipRule::TipLagTolerated},        // pong is deferred via Peer::m_pending_pong
    {NetMsgType::PONG,         TipRule::Nothing},                // RTT measurement
    {NetMsgType::NOTFOUND,     TipRule::Nothing},                // in-flight requests
    {NetMsgType::FILTERLOAD,   TipRule::Nothing},                // peer setting
    {NetMsgType::FILTERADD,    TipRule::Nothing},                // peer setting
    {NetMsgType::FILTERCLEAR,  TipRule::Nothing},                // peer setting
    {NetMsgType::SENDHEADERS,  TipRule::Nothing},                // peer setting
    {NetMsgType::FEEFILTER,    TipRule::Nothing},                // peer setting
    {NetMsgType::SENDCMPCT,    TipRule::Nothing},                // peer setting
    {NetMsgType::CMPCTBLOCK,   TipRule::PendingBlocksConnected}, // accepted as compact block only within two blocks of tip
    {NetMsgType::GETBLOCKTXN,  TipRule::TipLagTolerated},        // answered from the requested block directly
    {NetMsgType::BLOCKTXN,     TipRule::PendingBlocksConnected}, // completes a block processed like BLOCK
    {NetMsgType::GETCFILTERS,  TipRule::PendingBlocksConnected}, // stop block must pass BlockRequestAllowed()
    {NetMsgType::CFILTER,      TipRule::Nothing},                // not processed
    {NetMsgType::GETCFHEADERS, TipRule::PendingBlocksConnected}, // stop block must pass BlockRequestAllowed()
    {NetMsgType::CFHEADERS,    TipRule::Nothing},                // not processed
    {NetMsgType::GETCFCHECKPT, TipRule::PendingBlocksConnected}, // stop block must pass BlockRequestAllowed()
    {NetMsgType::CFCHECKPT,    TipRule::Nothing},                // not processed
    {NetMsgType::WTXIDRELAY,   TipRule::Nothing},                // handshake
    {NetMsgType::SENDTXRCNCL,  TipRule::Nothing},                // handshake
    {NetMsgType::FEATURE,      TipRule::Nothing},                // handshake
};
// clang-format on

static_assert(std::size(MESSAGE_TIP_RULES) == std::tuple_size_v<decltype(ALL_NET_MESSAGE_TYPES)>);

/** Return the TipRule for a known message type, or nullopt for unknown types. */
constexpr std::optional<TipRule> TipRuleOf(std::string_view type)
{
    for (const MessageTipRule& row : MESSAGE_TIP_RULES) {
        if (row.type == type) return row.rule;
    }
    return std::nullopt;
}

} // namespace node

#endif // BITCOIN_NODE_MESSAGE_TIP_RULE_H
