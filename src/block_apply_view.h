// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_BLOCK_APPLY_VIEW_H
#define BITCOIN_BLOCK_APPLY_VIEW_H

#include <coinsstore.h>
#include <primitives/block.h>
#include <undo.h>
#include <util/not_null.h>
#include <util/threadpool.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

/**
 * Outputs created by unpublished blocks in a parallel connect window.
 */
class WindowCoinSource
{
public:
    virtual ~WindowCoinSource() = default;

    //! Return true if an unpublished block below `viewer_height` (or an earlier tx `< own_limit` of
    //! `viewer_height` itself) defines `outpoint`'s txid, setting `coin` to the spendable output
    //! (or nullopt if that txid has no such spendable output).
    virtual bool FindCoin(const COutPoint& outpoint, int viewer_height, std::optional<Coin>& coin, uint32_t own_limit = 0) const = 0;

    //! Batch version of FindCoin(): invokes `found` for every input created by a lower unpublished
    //! block, and compacts `outpoints` in place to the remaining inputs that must be fetched from the store.
    virtual void FindCoins(std::vector<COutPoint>& outpoints,
                           int viewer_height,
                           const std::function<void(const COutPoint&, Coin&&)>& found) const = 0;

    //! Resolve each input by position into `coins`, setting `decided[i] = 1` when the window decides
    //! input `i` (including outputs of earlier transactions `< positions[i]` in the same block).
    virtual void FindInputs(std::span<const COutPoint> outpoints,
                            std::span<const uint32_t> positions,
                            int viewer_height,
                            std::vector<uint8_t>& decided,
                            std::vector<Coin>& coins) const
    {
        for (size_t i{0}; i < outpoints.size(); ++i) {
            std::optional<Coin> coin;
            if (!FindCoin(outpoints[i], viewer_height, coin, positions[i])) continue;
            decided[i] = 1;
            if (coin) coins[i] = std::move(*coin);
        }
    }

    //! Set `named[i] = 1` if an unpublished block below `viewer_height` may contain `txs[i]->GetHash()`.
    virtual void NamedIds(std::span<const CTransactionRef> txs, int viewer_height, std::vector<uint8_t>& named) const = 0;
};

/**
 * Per-block CoinsViewOverlay layered on top of a CoinsStore.
 *
 * Isolates a block's validation from the shared store so failed blocks leave nothing behind, and
 * commits validated changes to the store via CoinsStore::ApplyConfirmed().
 */
class BlockApplyView final : public CoinsViewOverlay
{
public:
    BlockApplyView(CoinsStore& store, util::NotNullSharedPtr<ThreadPool> thread_pool, bool deterministic = false);
    ~BlockApplyView() override;

    void SetHeight(int height) { m_height = height; }
    bool InUse() const { return m_height >= 0; }

    //! When true, misses read from the backing DB are cached cleanly in CoinsStore so the subsequent
    //! commit does not need a second DB read. Disabled at the tip so invalid blocks leave cache untouched.
    void SetKeepBaseReads(bool keep) { m_keep_base_reads = keep; }

    void SetOverlay(const WindowCoinSource* overlay, int height)
    {
        m_overlay = overlay;
        m_overlay_height = height;
    }

    void SetOwnLimit(uint32_t limit) { m_own_limit = limit; }

    //! Prefetch the block's inputs from the window overlay and CoinsStore in batched shard passes.
    void Prefetch(const CBlock& block);

    //! Resolve each input of `block` (excluding coinbase) by position into `coins`, leaving the view's map untouched.
    void ResolveInputs(const CBlock& block, std::vector<Coin>& coins);

    //! Bind the transaction being checked and its resolved input coins for address-matched lookups.
    void SetInputs(const CTransaction* tx, std::span<const Coin> coins)
    {
        m_inputs_tx = tx;
        m_inputs_coins = coins;
    }

    const Coin& AccessCoin(const COutPoint& outpoint) const override;
    bool HaveCoin(const COutPoint& outpoint) const override;
    bool SpendCoin(const COutPoint& outpoint, Coin* moveto = nullptr) override;
    bool HaveAnyOutput(const CBlock& block) const override;

    void Release() { Reset(); }
    CoinsStore& Store() const { return m_store; }

    //! Commit a serially validated block's changes to CoinsStore, using `undo` for any DB misses.
    void CommitAlone(const CBlock& block, const CBlockUndo& undo);

    //! Drop the view's local map after its changes have been extracted or committed.
    void ChangesCommitted();

    //! Advance CoinsStore's best block hash after publication and reset the view.
    void ConfirmCommitted();

    //! Compact representation of one change of a validated window block pointing into the CBlock.
    struct CommitItem {
        static constexpr uint8_t SPEND{1}, FRESH{2}, OVERWRITE{4}, KNOWN{8}, DROPPED{16}, CONFLICT{32};
        const void* at;
        uint32_t n;
        uint8_t flags;
        bool Spend() const { return (flags & SPEND) != 0; }
        const CTransaction& Tx() const { return *static_cast<const CTransaction*>(at); }
        COutPoint Outpoint() const { return Spend() ? *static_cast<const COutPoint*>(at) : COutPoint{Tx().GetHash(), n}; }
    };

protected:
    std::optional<Coin> ReadBase(const COutPoint& outpoint) const override;
    void Reset() noexcept override;

private:
    const Coin* InputCoin(const COutPoint& outpoint) const;

    CoinsStore& m_store;
    bool m_keep_base_reads{false};
    const WindowCoinSource* m_overlay{nullptr};
    int m_overlay_height{0};
    uint32_t m_own_limit{0};
    int m_height{-1};
    const CTransaction* m_inputs_tx{nullptr};
    std::span<const Coin> m_inputs_coins{};
};

#endif // BITCOIN_BLOCK_APPLY_VIEW_H
