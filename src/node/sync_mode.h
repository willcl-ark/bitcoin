// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_NODE_SYNC_MODE_H
#define BITCOIN_NODE_SYNC_MODE_H

#include <cstdint>
#include <string_view>

namespace node {
/**
 * Which of two ways the node processes blocks: what the initial block download
 * latch (ChainstateManager::IsInitialBlockDownload()) selects.
 *
 * The latch falls once, under cs_main, where a block becomes the tip
 * (Chainstate::PublishConnectTip()). An operation that spans more than one
 * hold of cs_main, or holds none, reads it once, where it starts
 * (ChainstateManager::SyncModeNow()), and hands the copy down: it finishes in
 * the mode it started in, and no two of its decisions are made on different
 * readings. The operations: a message off the wire
 * (PeerManagerImpl::ProcessMessage()), a step of an activation
 * (Chainstate::ActivateBestChain()), a pass of block requests to a peer
 * (PeerManagerImpl::SendMessages()).
 *
 * MODE_CHOICES lists everything the mode selects and the ordering precondition
 * of validation.h each choice of the download departs from. Whatever else
 * reads the latch selects nothing between blocks in flight, and reads it
 * afresh:
 *  - under the hold of cs_main that may change it, where the reading cannot
 *    turn before the hold ends (the signals and notifications of
 *    PublishConnectTip(), FinishConnectStep() and NotifyTipAdvanced(), and
 *    NewPoWValidBlock in AcceptBlock());
 *  - for a transaction, which is one hold of cs_main (the settle before
 *    AcceptToMemoryPool(), precondition O7);
 *  - for what is the same in both modes but for its cost or its log
 *    (compaction, the block bins' release, cache sizes, log levels, the fee
 *    filter, transaction announcements, the headers a compact block asks for).
 */
enum class SyncMode : uint8_t {
    //! Initial block download: blocks are received, connected and written beside each other.
    Download,
    //! Out of it: a block is connected before what follows it is processed, as on master.
    Tip,
};

constexpr SyncMode SyncModeOf(bool initial_block_download)
{
    return initial_block_download ? SyncMode::Download : SyncMode::Tip;
}

/** What the mode selects. */
enum class ModeChoice : uint8_t {
    ConnectWindow,
    ReceiveHandOff,
    PeerRequestRule,
    HandlerWaits,
    ActivationOnGetData,
    ActivationOnGetBlocks,
    HistoricalYield,
};

struct ModeChoiceRow {
    ModeChoice choice;
    //! Where the copy is read that decides it.
    std::string_view read;
    std::string_view download;
    std::string_view tip;
    //! The precondition the download's choice departs from, and what holds it instead.
    std::string_view departs;
};

inline constexpr ModeChoiceRow MODE_CHOICES[]{
    {ModeChoice::ConnectWindow,
     "ActivateBestChain(), where a step is planned",
     "the step connects a window of blocks beside each other",
     "the step connects one block after the other",
     "O6, O9: held by publishing the window's blocks in order (PublishConnectTip())"},
    {ModeChoice::ReceiveHandOff,
     "ProcessMessage(), for the block message",
     "a requested block is deserialized, checked and stored by a task of the connect pool",
     "the message handler does it",
     "O5: the request stays in flight until the block is stored (HandOffBlock())"},
    {ModeChoice::PeerRequestRule,
     "SendMessages(), for the pass's block requests",
     "a peer is asked for as many blocks as its deliveries allow, the sync peers only",
     "every peer may be asked, up to the fixed number in transit",
     "none: a choice of what to ask for"},
    {ModeChoice::HandlerWaits,
     "ProcessMessage(), for the message (node/message_tip_rule.h)",
     "the handler waits neither for the receive tasks nor for the connector",
     "it waits for both before a message that is judged against the tip",
     "O1: no transaction is accepted, and blocks are stored beside each other on purpose"},
    {ModeChoice::ActivationOnGetData,
     "ProcessGetBlockData(), afresh where the request is answered: a request kept over passes has no start",
     "a block stored and not connected is refused as one not stored",
     "the handler activates the chain first",
     "O1: this node announced no block, the connector activates"},
    {ModeChoice::ActivationOnGetBlocks,
     "ProcessMessage(), for the getblocks message",
     "the answer is read from what is connected",
     "the handler activates the chain first",
     "O1: as for a block request"},
    {ModeChoice::HistoricalYield,
     "ActivateBestChain(), where a step is planned; the connector, after the historical pass",
     "the historical chainstate's activation runs on",
     "it gives way to a block queued for the active chainstate",
     "none: nobody waits for the active chainstate's block"},
};
} // namespace node

#endif // BITCOIN_NODE_SYNC_MODE_H
