// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_NODE_CONNECT_MACHINE_H
#define BITCOIN_NODE_CONNECT_MACHINE_H

#include <util/machine.h>

#include <cstdint>

namespace node {
/**
 * A block of the connect window between its staging and its publication, as the
 * connector thread and the pool's workers see it of each other:
 *
 *   Reading --Index----------> Indexed --StartValidate--> Validating --Finish--> Done
 *      |   \                                                  ^
 *      |    +--IndexAndValidate-------------------------------+
 *      +-------FailRead--------------------------------------------------------> Failed
 *
 * Reading: block bytes are being read/parsed and its outputs indexed into the
 * window outputs overlay.
 * Indexed: outputs are in the window overlay; waiting for lower in-flight blocks
 * to finish indexing before single-pass validation starts.
 * Validating: a worker is validating the block read-only against the confirmed
 * CoinsStore + window overlay (single pass, zero deferred transactions).
 * Done: validation finished; outcome and net changes are ready for ordered
 * shard-parallel commit.
 * Failed: block bytes could not be read or parsed; no higher block may start
 * validating past it.
 */
enum class WindowBlockState : uint8_t {
    Reading,
    Indexed,
    Validating,
    Done,
    Failed,
};

enum class WindowBlockEvent : uint8_t {
    Index,
    IndexAndValidate,
    FailRead,
    StartValidate,
    Finish,
};

inline constexpr auto WINDOW_BLOCK_TABLE{util::MakeTransitionTable<WindowBlockState, WindowBlockEvent>({
    {WindowBlockState::Reading, WindowBlockEvent::Index, WindowBlockState::Indexed},
    {WindowBlockState::Reading, WindowBlockEvent::IndexAndValidate, WindowBlockState::Validating},
    {WindowBlockState::Reading, WindowBlockEvent::FailRead, WindowBlockState::Failed},
    {WindowBlockState::Indexed, WindowBlockEvent::StartValidate, WindowBlockState::Validating},
    {WindowBlockState::Validating, WindowBlockEvent::Finish, WindowBlockState::Done},
})};

//! The block's outputs are indexed in the window overlay: higher blocks need not wait for it to start validating.
constexpr bool IsIndexed(WindowBlockState state)
{
    return state == WindowBlockState::Indexed || state == WindowBlockState::Validating || state == WindowBlockState::Done;
}
//! The block's outcome is complete (validated or read-failed): the connector may process it.
constexpr bool IsComplete(WindowBlockState state)
{
    return state == WindowBlockState::Done || state == WindowBlockState::Failed;
}
//! A worker is currently validating the block (separate from its initial parse task).
constexpr bool IsValidating(WindowBlockState state) { return state == WindowBlockState::Validating; }
} // namespace node

#endif // BITCOIN_NODE_CONNECT_MACHINE_H
