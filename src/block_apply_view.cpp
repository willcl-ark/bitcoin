// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <block_apply_view.h>

#include <util/check.h>
#include <util/hasher.h>

#include <unordered_map>
#include <utility>
#include <vector>

BlockApplyView::BlockApplyView(CoinsStore& store, util::NotNullSharedPtr<ThreadPool> thread_pool, bool deterministic)
    : CoinsViewOverlay(&store, std::move(thread_pool), deterministic), m_store{store}
{
}

BlockApplyView::~BlockApplyView()
{
    Reset();
}

void BlockApplyView::ResolveInputs(const CBlock& block, std::vector<Coin>& coins)
{
    static thread_local std::vector<COutPoint> outpoints;
    static thread_local std::vector<uint32_t> positions;
    outpoints.clear();
    positions.clear();
    for (size_t i{1}; i < block.vtx.size(); ++i) {
        for (const CTxIn& in : block.vtx[i]->vin) {
            outpoints.push_back(in.prevout);
            positions.push_back(static_cast<uint32_t>(i));
        }
    }
    coins.assign(outpoints.size(), Coin{});
    if (outpoints.empty()) return;
    static thread_local std::vector<uint8_t> decided;
    decided.assign(outpoints.size(), 0);
    if (m_overlay) {
        m_overlay->FindInputs(outpoints, positions, m_overlay_height, decided, coins);
    }
    static thread_local std::vector<uint32_t> rest;
    rest.clear();
    for (uint32_t i{0}; i < outpoints.size(); ++i) {
        if (!decided[i]) rest.push_back(i);
    }
    static thread_local std::vector<uint8_t> found;
    m_store.FetchInto(outpoints, rest, coins, found);
    for (const uint32_t i : rest) {
        if (found[i]) continue;
        std::optional<Coin> coin{m_keep_base_reads ? m_store.GetCoin(outpoints[i]) : m_store.PeekCoin(outpoints[i])};
        if (coin) coins[i] = std::move(*coin);
    }
}

void BlockApplyView::Prefetch(const CBlock& block)
{
    size_t inputs{0};
    for (size_t i{1}; i < block.vtx.size(); ++i) inputs += block.vtx[i]->vin.size();
    std::vector<COutPoint> outpoints;
    outpoints.reserve(inputs);
    for (size_t i{1}; i < block.vtx.size(); ++i) {
        for (const CTxIn& in : block.vtx[i]->vin) {
            outpoints.push_back(in.prevout);
        }
    }
    if (m_overlay) {
        m_overlay->FindCoins(outpoints, m_overlay_height, [&](const COutPoint& outpoint, Coin&& coin) {
            const size_t usage{coin.DynamicMemoryUsage()};
            if (cacheCoins.try_emplace(outpoint, std::move(coin)).second) cachedCoinsUsage += usage;
        });
    }
    for (auto& [outpoint, coin] : m_store.Fetch(std::move(outpoints))) {
        const size_t usage{coin.DynamicMemoryUsage()};
        if (cacheCoins.try_emplace(outpoint, std::move(coin)).second) cachedCoinsUsage += usage;
    }
}

bool BlockApplyView::SpendCoin(const COutPoint& outpoint, Coin* moveto)
{
    Coin local;
    Coin& spent{moveto ? *moveto : local};
    if (!CoinsViewOverlay::SpendCoin(outpoint, &spent)) return false;
    // Preserve the spent coin's creation height and coinbase flag on the tombstone so ApplyConfirmed()
    // can verify the store still holds the exact coin the block validated against.
    if (const auto it{cacheCoins.find(outpoint)}; it != cacheCoins.end()) {
        it->second.coin.nHeight = spent.nHeight;
        it->second.coin.fCoinBase = spent.fCoinBase;
    }
    return true;
}

bool BlockApplyView::HaveAnyOutput(const CBlock& block) const
{
    if (!m_overlay) return CoinsViewOverlay::HaveAnyOutput(block);
    const std::span<const CTransactionRef> txs{block.vtx};
    static thread_local std::vector<uint8_t> named;
    m_overlay->NamedIds(txs, m_overlay_height, named);
    std::vector<COutPoint> outpoints;
    for (size_t i{0}; i < txs.size(); ++i) {
        const CTransaction& tx{*txs[i]};
        for (uint32_t o{0}; o < tx.vout.size(); ++o) {
            const COutPoint outpoint{tx.GetHash(), o};
            if (named[i]) {
                if (HaveCoin(outpoint)) return true;
            } else if (const auto it{cacheCoins.find(outpoint)}; it != cacheCoins.end()) {
                if (!it->second.coin.IsSpent()) return true;
            } else {
                outpoints.push_back(outpoint);
            }
        }
    }
    static thread_local std::vector<uint8_t> may;
    m_store.MayHave(outpoints, may);
    for (size_t i{0}; i < outpoints.size(); ++i) {
        if (may[i] && HaveCoin(outpoints[i])) return true;
    }
    return false;
}

void BlockApplyView::CommitAlone(const CBlock& block, const CBlockUndo& undo)
{
    std::vector<CoinsStore::Change> changes;
    changes.reserve(m_dirty_count);
    const uint32_t own_height{static_cast<uint32_t>(m_height)};
    for (auto& [outpoint, entry] : cacheCoins) {
        if (!entry.IsDirty()) continue;
        const bool known_coin{entry.coin.IsSpent() && entry.coin.nHeight != own_height};
        const bool may_overwrite{!entry.IsFresh() && !entry.coin.IsSpent() && entry.coin.IsCoinBase()};
        changes.push_back({
            .outpoint = &outpoint,
            .coin = &entry.coin,
            .possible_overwrite = may_overwrite,
            .known_coin = known_coin,
        });
    }
    std::unordered_map<COutPoint, const Coin*, SaltedOutpointHasher> spent;
    const auto base_coin{[&](const COutPoint& outpoint) -> const Coin* {
        if (spent.empty() && undo.vtxundo.size() + 1 == block.vtx.size()) {
            for (size_t i{1}; i < block.vtx.size(); ++i) {
                const CTransaction& tx{*block.vtx[i]};
                const CTxUndo& tx_undo{undo.vtxundo[i - 1]};
                if (tx_undo.vprevout.size() != tx.vin.size()) continue;
                for (size_t j{0}; j < tx.vin.size(); ++j) spent.emplace(tx.vin[j].prevout, &tx_undo.vprevout[j]);
            }
        }
        const auto it{spent.find(outpoint)};
        return it == spent.end() ? nullptr : it->second;
    }};
    Assert(m_store.ApplyConfirmed(std::move(changes), /*report=*/nullptr, base_coin) == 0);
    ChangesCommitted();
}

void BlockApplyView::ChangesCommitted()
{
    const size_t entries{cacheCoins.size()};
    cacheCoins.clear();
    ReallocateCache();
    cacheCoins.reserve(entries);
    cachedCoinsUsage = 0;
    m_dirty_count = 0;
}

void BlockApplyView::ConfirmCommitted()
{
    m_store.SetBestBlock(GetBestBlock());
    Reset();
}

const Coin* BlockApplyView::InputCoin(const COutPoint& outpoint) const
{
    if (!m_inputs_tx || m_inputs_tx->vin.empty()) return nullptr;
    const std::vector<CTxIn>& vin{m_inputs_tx->vin};
    const auto at{reinterpret_cast<uintptr_t>(&outpoint)};
    const auto first{reinterpret_cast<uintptr_t>(&vin.front().prevout)};
    if (at < first) return nullptr;
    const size_t i{(at - first) / sizeof(CTxIn)};
    if (i >= vin.size() || &vin[i].prevout != &outpoint) return nullptr;
    return &m_inputs_coins[i];
}

const Coin& BlockApplyView::AccessCoin(const COutPoint& outpoint) const
{
    if (const Coin* coin{InputCoin(outpoint)}) return *coin;
    return CoinsViewOverlay::AccessCoin(outpoint);
}

bool BlockApplyView::HaveCoin(const COutPoint& outpoint) const
{
    if (const Coin* coin{InputCoin(outpoint)}) return !coin->IsSpent();
    return CoinsViewOverlay::HaveCoin(outpoint);
}

std::optional<Coin> BlockApplyView::ReadBase(const COutPoint& outpoint) const
{
    if (m_overlay) {
        std::optional<Coin> coin;
        if (m_overlay->FindCoin(outpoint, m_overlay_height, coin, m_own_limit)) return coin;
    }
    return m_keep_base_reads ? m_store.GetCoin(outpoint) : m_store.PeekCoin(outpoint);
}

void BlockApplyView::Reset() noexcept
{
    CoinsViewOverlay::Reset();
    m_inputs_tx = nullptr;
    m_inputs_coins = {};
    m_keep_base_reads = false;
    m_overlay = nullptr;
    m_overlay_height = 0;
    m_own_limit = 0;
    m_height = -1;
}
