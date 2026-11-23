// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_COINS_ENTRY_MACHINE_H
#define BITCOIN_COINS_ENTRY_MACHINE_H

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

/**
 * The state of one confirmed coin entry in the coins store (CoinsStore::Entry),
 * and every way it changes.
 *
 * The store holds confirmed UTXO state only: blocks in the connect window
 * validate read-only against the store plus a small window outputs overlay, and
 * validated prefixes commit in height order per shard. An entry therefore needs
 * only three stored flags (DIRTY, FRESH, COLLECTED) plus whether its coin is
 * spent (SPENT, read from the coin itself).
 *
 * RULES lists, per event, the states it applies to, the bits it clears and
 * sets, and what it does to the height the entry is filed under (Height). NEXT
 * is built during compilation and refuses to build if two rules of one event
 * overlap or if any rule leads outside the known states.
 */
namespace coins_entry {
using State = uint8_t;

//! The entry differs from the database: it is on a dirty list, for a write-out run to find.
inline constexpr State DIRTY{1 << 0};
//! The database is known not to hold the outpoint, so a spend leaves nothing to erase.
inline constexpr State FRESH{1 << 1};
//! A run collected the entry's value and has not settled it; any change clears the bit, and the entry
//! then stays dirty when the run's chunk settles.
inline constexpr State COLLECTED{1 << 2};
//! The coin is spent. Not stored with the flags: the coin says it.
inline constexpr State SPENT{1 << 7};

//! The flags an entry stores.
inline constexpr State FLAGS{DIRTY | FRESH | COLLECTED};
//! An entry just inserted: no coin and nothing to say. Every insertion is followed by an event.
inline constexpr State ABSENT{SPENT};
//! A clean, unspent copy of what the database holds.
inline constexpr State BASE_COPY{0};
//! No state: what NEXT answers for an event the machine refuses.
inline constexpr State REFUSED{0xff};

//! Whether an entry of the store may rest in `s`.
constexpr bool IsStored(State s)
{
    if ((s & ~(FLAGS | SPENT)) != 0) return false;
    const bool dirty(s & DIRTY), fresh(s & FRESH), collected(s & COLLECTED), spent(s & SPENT);
    // A clean entry is an unspent copy of the database's coin and nothing else.
    if (!dirty) return s == BASE_COPY;
    // A fresh entry is an unspent coin not yet collected for the database.
    if (fresh && (spent || collected)) return false;
    return true;
}

//! Whether the machine knows `s`: a state an entry may rest in, or the one it is inserted in.
constexpr bool IsKnown(State s) { return s == ABSENT || IsStored(s); }

enum class Event : uint8_t {
    //! A lookup brings in the database's coin.
    BaseCopy,
    //! A confirmed coin the database is known not to hold.
    ConfirmedNew,
    //! A confirmed coin written over whatever was there, of which the database may hold something.
    ConfirmedWrite,
    //! As ConfirmedWrite, from a child view's batch over an entry that was there: one the database is
    //! known not to hold stays so.
    BatchOverwrite,
    //! A confirmed spend of an entry the database may hold: the erase is to be written.
    ConfirmedSpend,
    //! A run wrote a coin a disconnect restored: the database holds the outpoint now.
    KeptWritten,
    //! A partial write-out wrote the erase owed under a newer coin, moving the entry up to the coin's height.
    RunWritesOwed,
    //! A run collects the entry's value for the database.
    RunCollects,
    //! The run's chunk is in the database and the entry was not changed since.
    RunLands,
    //! A flush of the whole tier clears COLLECTED on any entry it walks.
    Flush,
};
inline constexpr size_t EVENT_COUNT{static_cast<size_t>(Event::Flush) + 1};

/** What an event does to the height an entry is filed under. */
enum class Height : uint8_t {
    //! The entry stays where it is filed; the event's height, if it has one, is not looked at.
    Keep,
    //! The entry is filed under the event's height.
    Take,
    //! As Take, unless the entry is already dirty and not fresh (so it may owe the database an erase
    //! from an earlier height): keep the lower of the two heights so no partial run skips the owed erase.
    Lower,
};

struct Rule {
    Event event;
    State all_of;
    State none_of;
    State clear;
    State set;
    Height height;
};

inline constexpr State EVERYTHING{0xff};

// clang-format off
inline constexpr Rule RULES[]{
    // event                  all_of            none_of        clear                set            height
    {Event::BaseCopy,         SPENT,            FLAGS,         EVERYTHING,          BASE_COPY,     Height::Take},
    {Event::ConfirmedNew,     SPENT,            FLAGS,         EVERYTHING,          DIRTY | FRESH, Height::Take},
    {Event::ConfirmedWrite,   0,                0,             EVERYTHING,          DIRTY,         Height::Lower},
    {Event::BatchOverwrite,   DIRTY,            0,             EVERYTHING & ~FRESH, DIRTY,         Height::Lower},
    {Event::BatchOverwrite,   0,                DIRTY | SPENT, EVERYTHING,          DIRTY,         Height::Lower},
    {Event::ConfirmedSpend,   0,                FRESH,         EVERYTHING,          DIRTY | SPENT, Height::Lower},
    {Event::KeptWritten,      0,                0,             FRESH,               0,             Height::Keep},
    {Event::RunWritesOwed,    DIRTY,            SPENT | FRESH, COLLECTED,           0,             Height::Take},
    {Event::RunCollects,      DIRTY,            0,             FRESH,               COLLECTED,     Height::Keep},
    {Event::RunLands,         DIRTY | COLLECTED,0,             DIRTY | COLLECTED,   0,             Height::Keep},
    {Event::Flush,            0,                0,             COLLECTED,           0,             Height::Keep},
};
// clang-format on

using NextTable = std::array<std::array<State, 256>, EVENT_COUNT>;
using HeightTable = std::array<std::array<Height, 256>, EVENT_COUNT>;

consteval NextTable MakeNextTable()
{
    NextTable table{};
    for (auto& row : table) {
        for (State& next : row) next = REFUSED;
    }
    for (const Rule& rule : RULES) {
        auto& row{table[static_cast<size_t>(rule.event)]};
        for (size_t s{0}; s < 256; ++s) {
            const State from{static_cast<State>(s)};
            if (!IsKnown(from)) continue;
            if ((from & rule.all_of) != rule.all_of || (from & rule.none_of) != 0) continue;
            if (row[s] != REFUSED) throw "two rules of one event apply to one state";
            const State to{static_cast<State>((from & ~rule.clear) | rule.set)};
            if (!IsKnown(to)) throw "a rule leads out of the states an entry may be in";
            row[s] = to;
        }
    }
    return table;
}

//! The state `event` leads to from `from`, or REFUSED.
inline constexpr NextTable NEXT{MakeNextTable()};
static_assert(!IsKnown(REFUSED));

constexpr State Next(State from, Event event) { return NEXT[static_cast<size_t>(event)][from]; }

consteval HeightTable MakeHeightTable()
{
    HeightTable table{};
    for (auto& row : table) {
        for (Height& height : row) height = Height::Keep;
    }
    for (const Rule& rule : RULES) {
        auto& row{table[static_cast<size_t>(rule.event)]};
        for (size_t s{0}; s < 256; ++s) {
            const State from{static_cast<State>(s)};
            if (!IsKnown(from)) continue;
            if ((from & rule.all_of) != rule.all_of || (from & rule.none_of) != 0) continue;
            row[s] = rule.height;
        }
    }
    return table;
}

//! What `event` does in `from` to the height the entry is filed under. Keep where NEXT refuses.
inline constexpr HeightTable HEIGHT{MakeHeightTable()};

constexpr Height HeightRule(State from, Event event) { return HEIGHT[static_cast<size_t>(event)][from]; }

/**
 * The height an entry is filed under after an event: `rule` is what the event does (HeightRule()), `from`
 * the state before it, `filed` the height the entry is filed under now and `event_height` the event's.
 */
constexpr std::optional<int> NextHeight(Height rule, State from, int filed, int event_height)
{
    switch (rule) {
    case Height::Keep: return filed;
    case Height::Take: return event_height;
    case Height::Lower:
        return (from & DIRTY) && !(from & (FRESH | COLLECTED)) ? std::min(filed, event_height) : event_height;
    } // no default case, so the compiler can warn about missing cases
    return std::nullopt;
}
} // namespace coins_entry

#endif // BITCOIN_COINS_ENTRY_MACHINE_H
