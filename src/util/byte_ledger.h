// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_UTIL_BYTE_LEDGER_H
#define BITCOIN_UTIL_BYTE_LEDGER_H

#include <util/check.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace util {

/**
 * Memory accounts for blocks held in flight across asynchronous validation and I/O stages.
 */
enum class ByteAccountId : uint8_t {
    //! Blocks and views staged inside the parallel connect window.
    Window,
    //! Serialized undo data queued for background writing.
    UndoBacklog,
    //! Connected blocks pinned by pending validation signals waiting behind undo writes.
    PinnedBlocks,
};

inline constexpr std::array BYTE_ACCOUNTS{
    ByteAccountId::Window,
    ByteAccountId::UndoBacklog,
    ByteAccountId::PinnedBlocks,
};

//! One memory account. Safe to read from any thread; mutations are ordered by its holder.
class ByteAccount
{
    std::atomic<size_t> m_bytes{0};
    std::atomic<size_t> m_limit{0};

public:
    void Add(size_t bytes)
    {
        const size_t prev{m_bytes.fetch_add(bytes, std::memory_order_relaxed)};
        Assert(prev <= std::numeric_limits<size_t>::max() - bytes);
    }

    void Sub(size_t bytes)
    {
        const size_t prev{m_bytes.fetch_sub(bytes, std::memory_order_relaxed)};
        Assert(prev >= bytes);
    }

    void Set(size_t bytes) { m_bytes.store(bytes, std::memory_order_relaxed); }

    //! Zero means no byte limit is enforced.
    void SetLimit(size_t limit) { m_limit.store(limit, std::memory_order_relaxed); }

    size_t Bytes() const { return m_bytes.load(std::memory_order_relaxed); }
    size_t Limit() const { return m_limit.load(std::memory_order_relaxed); }

    //! Overflow-safe check whether adding `more` bytes stays within the configured limit.
    bool Admits(size_t more = 0) const
    {
        const size_t limit{Limit()};
        if (limit == 0) return true;
        const size_t cur{Bytes()};
        return cur <= limit && more <= limit - cur;
    }
};

class ByteLedger
{
    std::array<ByteAccount, BYTE_ACCOUNTS.size()> m_accounts;

public:
    ByteAccount& operator[](ByteAccountId id) { return m_accounts[static_cast<size_t>(id)]; }
    const ByteAccount& operator[](ByteAccountId id) const { return m_accounts[static_cast<size_t>(id)]; }
};

} // namespace util

#endif // BITCOIN_UTIL_BYTE_LEDGER_H
