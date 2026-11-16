// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_NODE_HANDOFF_MACHINE_H
#define BITCOIN_NODE_HANDOFF_MACHINE_H

#include <util/check.h>
#include <util/machine.h>

#include <cstdint>

namespace node {
/**
 * A block between its arrival and the activation that connects it, as the threads
 * that wait for it see it. The thread that accepts a block does not connect it:
 * it hands the block to the connector thread and goes on.
 *
 *   WithReceiveTask --Store--> Queued --Take--> Taken --ActiveAttempted--> DoneActive --AllAttempted--> Done
 *      |       |                  |               |                            |
 *    Refuse   Drop                +-----Stop------+------------Stop------------+--> Stopped
 *      v       v
 *   Refused  Dropped
 *
 * WithReceiveTask: in initial block download a task of the connect pool
 * deserializes, checks and stores the block (PeerManagerImpl::HandOffBlock()).
 * Refused: the task checked the block and did not store it (bad, mutated, or a
 * copy). Dropped: the task did not process it (its peer is going, or the bytes
 * are no block); the request is removed for the block to be asked for again.
 * Queued: stored and handed to the connector; a block accepted on the message
 * handler starts here. Taken: the connector's activation that covers it is
 * running. DoneActive: the active chainstate's activation has been attempted.
 * Done: so has the historical chainstate's, if there is one. Stopped: the
 * connector was stopped first, and nobody is to wait for the block.
 *
 * Out of initial block download the historical chainstate's activation gives
 * way to a block queued for the active one
 * (ChainstateManager::YieldHistoricalActivation()). The blocks of that pass
 * stay DoneActive while the connector takes again, and are Done with the
 * blocks of the pass that follows.
 *
 * Who waits for what:
 *  - a peer's ping is answered once none of its blocks is WithReceiveTask and
 *    every block handed over by then is Done (Peer::m_pending_pong);
 *  - a transaction (a peer's, or an orphan considered again), a compact
 *    block's distance from the tip and a request for headers are judged
 *    once every block handed over before that the active chainstate may
 *    connect is DoneActive (ChainstateManager::WaitForPendingBlocks()); so
 *    is a block (BLOCK, or the BLOCKTXN that completes one), which finds
 *    the block before it connected. Outside initial block download only:
 *    in it the wait returns at once. A block that is still WithReceiveTask
 *    when the download ends is waited for first
 *    (PeerManagerImpl::WaitForPendingBlocks());
 *  - a stalling verdict is given once no block is WithReceiveTask, Queued, Taken
 *    or DoneActive (PeerManagerImpl::SendMessages()).
 * A fourth waiter waits for no machine: a peer's request for a block
 * announced to it is kept while the block is being stored
 * (PeerManagerImpl::ProcessGetBlockData(),
 * ChainstateManager::IsBlockBeingStored()).
 *
 * The states of the receive task are the membership of its set
 * (PeerManagerImpl::m_block_tasks) and the event its task leaves with. Those
 * from Queued on are not stored per block either: the connector takes every
 * queued block at once, so a few marks say where each block stands
 * (ConnectorHandOffs).
 *
 * No site of the node asks the table, then: it is the specification of what
 * the set and the marks do, and handoff_machine_tests holds the marks to it
 * (every step ConnectorHandOffs takes is a transition of the table).
 */
enum class HandOffState : uint8_t {
    WithReceiveTask,
    Refused,
    Dropped,
    Queued,
    Taken,
    DoneActive,
    Done,
    Stopped,
};

enum class HandOffEvent : uint8_t {
    Store,
    Refuse,
    Drop,
    Take,
    ActiveAttempted,
    AllAttempted,
    Stop,
};

inline constexpr auto HAND_OFF_TABLE{util::MakeTransitionTable<HandOffState, HandOffEvent>({
    {HandOffState::WithReceiveTask, HandOffEvent::Store, HandOffState::Queued},
    {HandOffState::WithReceiveTask, HandOffEvent::Refuse, HandOffState::Refused},
    {HandOffState::WithReceiveTask, HandOffEvent::Drop, HandOffState::Dropped},
    {HandOffState::Queued, HandOffEvent::Take, HandOffState::Taken},
    {HandOffState::Taken, HandOffEvent::ActiveAttempted, HandOffState::DoneActive},
    {HandOffState::DoneActive, HandOffEvent::AllAttempted, HandOffState::Done},
    {HandOffState::Queued, HandOffEvent::Stop, HandOffState::Stopped},
    {HandOffState::Taken, HandOffEvent::Stop, HandOffState::Stopped},
    {HandOffState::DoneActive, HandOffEvent::Stop, HandOffState::Stopped},
})};

//! The blocks handed to the connector up to one of them: what a waiter keeps.
struct HandOffTicket {
    //! How many blocks were handed over, this one included.
    uint64_t all{0};
    //! How many of them the active chainstate may connect (not those at or below a snapshot's base).
    uint64_t active{0};
};

/**
 * The blocks handed to the connector, from Queued on. Every event but the hand
 * itself moves all blocks of a state at once, so the object keeps marks, not
 * blocks. Not synchronized: its owner's mutex guards it.
 */
class ConnectorHandOffs
{
    uint64_t m_handed{0};
    uint64_t m_handed_active{0};
    uint64_t m_taken{0};
    uint64_t m_taken_active{0};
    uint64_t m_done_active{0};
    uint64_t m_done{0};
    //! How many of all blocks handed over the active chainstate's activation has been attempted for: the
    //! taken blocks up to here are DoneActive. Not m_taken where a pass is between Take and ActiveAttempted.
    uint64_t m_active_attempted_all{0};
    bool m_stopped{false};

public:
    //! A block is handed over: it is Queued (or Stopped, once the connector is).
    HandOffTicket Hand(bool for_active)
    {
        ++m_handed;
        if (for_active) ++m_handed_active;
        return Latest();
    }
    //! The ticket of the block handed over last.
    HandOffTicket Latest() const { return {m_handed, m_handed_active}; }

    //! The connector takes every Queued block for one activation. The blocks it took before are Done, or
    //! DoneActive where the historical chainstate's activation gave way: those are Done with the blocks of
    //! this pass.
    void Take()
    {
        if (m_stopped) return;
        Assert(m_active_attempted_all == m_taken);
        m_taken = m_handed;
        m_taken_active = m_handed_active;
    }
    //! The active chainstate's activation was attempted: every Taken block is DoneActive.
    void ActiveAttempted()
    {
        if (m_stopped) return;
        m_done_active = m_taken_active;
        m_active_attempted_all = m_taken;
    }
    //! The historical chainstate's activation was attempted too: every DoneActive block is Done.
    void AllAttempted()
    {
        if (m_stopped) return;
        Assert(m_active_attempted_all == m_taken);
        m_done = m_taken;
    }
    //! The connector stops: every block not Done is Stopped and stays so.
    void Stop() { m_stopped = true; }

    bool Stopped() const { return m_stopped; }
    //! Whether a block is Queued: the connector has an activation to run.
    bool AnyQueued() const { return m_handed > m_taken; }
    //! Whether a block the active chainstate may connect is Queued.
    bool AnyActiveQueued() const { return m_handed_active > m_taken_active; }
    //! Whether a block is not Done yet.
    bool AnyUnattempted() const { return m_handed > m_done; }
    //! Whether every block up to the ticket's that the active chainstate may connect is DoneActive or
    //! Done. A block it may not connect is not waited for, wherever it stands.
    bool ActiveChainAttempted(const HandOffTicket& ticket) const { return ticket.active <= m_done_active; }
    //! Whether every block up to the ticket's is Done.
    bool Attempted(const HandOffTicket& ticket) const { return ticket.all <= m_done; }

    //! Where the block that was handed over with `ticket` stands.
    HandOffState StateOf(const HandOffTicket& ticket) const
    {
        if (ticket.all <= m_done) return HandOffState::Done;
        if (m_stopped) return HandOffState::Stopped;
        if (ticket.all > m_taken) return HandOffState::Queued;
        return ticket.all <= m_active_attempted_all ? HandOffState::DoneActive : HandOffState::Taken;
    }
};
} // namespace node

#endif // BITCOIN_NODE_HANDOFF_MACHINE_H
