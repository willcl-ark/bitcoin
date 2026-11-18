// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_NODE_DURABILITY_MACHINE_H
#define BITCOIN_NODE_DURABILITY_MACHINE_H

namespace node {
/**
 * Durability of one block's bytes in a block file or an undo file, as far as
 * the block index may rely on them: one state for its data (BLOCK_HAVE_DATA)
 * and one for its undo (BLOCK_HAVE_UNDO).
 *
 * The block index database never takes the flag for Unsynced bytes. An index
 * entry that survives a crash must never point at bytes that did not.
 *
 * The state is derived, not stored: the flag in nStatus says whether the
 * bytes exist, and the block manager's sets of files written since their
 * last sync say whether they are safe. The sets are per file, so a block
 * whose own bytes were synced reads as Unsynced again when a later block is
 * written into the same file. That errs on the safe side.
 *
 * A failed sync leaves the block Unsynced for the rest of the run; the flush
 * that wanted to record it, and every later one, reports the failure and
 * does not reach the index write.
 *
 * The guarantee is per run. Synced means synced since startup or recorded by
 * an earlier run under the same rule; a block index written by a version
 * without the rule is taken as it is, and so are the block files of a
 * reindex, which records blocks an earlier run wrote without a sync of
 * their files in this one.
 */
enum class DurabilityState {
    Absent,   //!< No bytes the index could refer to (never written, or pruned).
    Unsynced, //!< Written, the file not synced since.
    Synced,   //!< Written, and the file synced since.
};
} // namespace node

#endif // BITCOIN_NODE_DURABILITY_MACHINE_H
