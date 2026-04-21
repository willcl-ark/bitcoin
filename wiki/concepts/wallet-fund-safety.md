---
kind: concept
title: Wallet Fund Safety
status: active
last_reviewed: 2026-04-21
paths:
  - src/wallet/spend.cpp
  - src/wallet/wallet.cpp
  - src/wallet/receive.cpp
  - src/wallet/feebumper.cpp
  - src/wallet/rpc/spend.cpp
  - src/wallet/transaction.h
tags:
  - wallet
  - fund-safety
  - spending
  - balances
  - signing
---

# Wallet Fund Safety

## Summary

Wallet fund safety is enforced by a cluster of wallet-local checks around coin
eligibility, transaction construction, signing completeness, replacement
handling, and balance cache invalidation. The highest-risk paths are
`src/wallet/spend.cpp` (`AvailableCoins`, `FetchSelectedInputs`,
`CreateTransactionInternal`), `src/wallet/wallet.cpp`
(`SignTransaction`, `FillPSBT`, `CommitTransaction`),
`src/wallet/receive.cpp` (`GetBalance`), and `src/wallet/feebumper.cpp`
(`CreateRateBumpTransaction`).

Current code mixes hard checks with wallet policy and heuristics. Dust checks,
fee/weight bounds, signing-completeness checks, and replacement exclusions are
hard failures. Coin-selection preferences such as avoid-partial-spends,
zero-conf change spending, and avoid-reuse filtering are wallet policy knobs,
not consensus guarantees.

## Invariants

- `fund-loss`, `wrong-transaction`: `wallet::CreateTransaction()` rejects empty
  recipient sets and negative amounts, and `wallet::CreateTransactionInternal()`
  rejects dust recipients, invalid change positions, missing solving data for
  size estimation, negative fees, fee mismatches, oversized transactions, fees
  above `m_default_max_tx_fee`, and transactions that would fail
  `checkChainLimits()` when `-walletrejectlongchains` is enabled
  (`src/wallet/spend.cpp` (`CreateTransaction`, `CreateTransactionInternal`)).
- `fund-loss`, `wrong-transaction`: manually selected inputs are validated
  before use. `FetchSelectedInputs()` rejects missing preselected inputs and
  unsolvable external inputs, and `SelectCoins()` fails if preset coins do not
  cover the target when automatic input selection is disabled
  (`src/wallet/spend.cpp` (`FetchSelectedInputs`, `SelectCoins`)).
- `fund-loss`, `double-pay`, `stranded-funds`: spendable-coin filtering is a
  direct safety boundary. `AvailableCoins()` excludes spent and locked
  outpoints, zero-conf transactions not in the mempool, and unconfirmed
  transactions marked with `replaces_txid` or `replaced_by_txid`; by default it
  also excludes unsafe inputs and, when `WALLET_FLAG_AVOID_REUSE` is active,
  reused destinations (`src/wallet/spend.cpp` (`AvailableCoins`),
  `src/wallet/wallet.cpp` (`SetSpentKeyState`, `IsSpentKey`)). The unsafe-input
  and avoid-reuse parts are policy choices, not hard protocol rules.
- `fund-loss`, `double-pay`: fee-bump creation keeps all original inputs in the
  replacement transaction and forbids sourcing new unconfirmed inputs, because
  omitting original conflicts could let multiple bumps confirm and accidentally
  double pay (`src/wallet/feebumper.cpp`
  (`PreconditionChecks`, `CreateRateBumpTransaction`)).
- `wrong-transaction`, `stranded-funds`: automatic change uses a freshly
  reserved destination, and `CreateTransactionInternal()` only calls
  `ReserveDestination::KeepDestination()` after all construction checks pass.
  The same function still carries an explicit tradeoff comment: restoring an
  older backup may strand post-backup change if the backup predates the new
  change key (`src/wallet/spend.cpp` (`CreateTransactionInternal`)).
- `wrong-transaction`: `CWallet::SignTransaction()` returns `false` if an input
  prevout is missing from `mapWallet` or if no `ScriptPubKeyMan` fully signs
  every input. `CWallet::FillPSBT()` enriches inputs from wallet state, asks
  every `ScriptPubKeyMan` to contribute data, and only marks `complete=true`
  when `PSBTInputSignedAndVerified()` succeeds for every input
  (`src/wallet/wallet.cpp` (`SignTransaction`, `FillPSBT`)).
- `balance-miscompute`: wallet balance reporting is based on current unspent
  outputs, not an independent historical ledger. `GetBalance()` iterates
  `GetTXOs()`, excludes spent outputs, and classifies value using
  `CachedTxIsTrusted()`. Cache invalidation is therefore safety-critical:
  `CWalletTx::MarkDirty()` clears cached debit/credit/change state, and
  `CWallet::CommitTransaction()` marks spent inputs dirty before broadcast
  (`src/wallet/receive.cpp` (`GetBalance`, `CachedTxIsTrusted`),
  `src/wallet/transaction.h` (`CWalletTx::MarkDirty`),
  `src/wallet/wallet.cpp` (`CommitTransaction`, `MarkDirty`)).
- `stranded-funds`: `CWallet::CommitTransaction()` writes the new transaction to
  the wallet and updates spent-input state before trying immediate broadcast.
  If broadcast fails, the wallet logs the failure but does not roll back the
  local transaction record, so local accounting is preserved while network
  propagation remains best-effort (`src/wallet/wallet.cpp`
  (`CommitTransaction`)).

## Important code paths

- Spend RPC entry points:
  `src/wallet/rpc/spend.cpp` (`SendMoney`, `FundTransaction`,
  `FinishTransaction`, `signrawtransactionwithwallet`, `walletprocesspsbt`)
- Coin eligibility, preset-input validation, fee/change assembly:
  `src/wallet/spend.cpp` (`AvailableCoins`, `FetchSelectedInputs`,
  `AutomaticCoinSelection`, `SelectCoins`, `CreateTransactionInternal`,
  `FundTransaction`)
- Signing and PSBT completion:
  `src/wallet/wallet.cpp` (`SignTransaction`, `FillPSBT`,
  `TransactionChangeType`)
- Replacement safety:
  `src/wallet/feebumper.cpp` (`PreconditionChecks`,
  `CreateRateBumpTransaction`, `CommitTransaction`)
- Balance and trust accounting:
  `src/wallet/receive.cpp` (`CachedTxIsTrusted`, `GetBalance`)
- Cache invalidation for debits, credits, and change:
  `src/wallet/transaction.h` (`CWalletTx::MarkDirty`),
  `src/wallet/wallet.cpp` (`MarkDirty`, `CommitTransaction`,
  `SetSpentKeyState`, `MarkReplaced`)

## Related tests

- `src/wallet/test/spend_tests.cpp` (`SubtractFee`,
  `wallet_duplicated_preset_inputs_test`)
- `src/wallet/test/psbt_wallet_tests.cpp` (`psbt_updater_test`)
- `test/functional/wallet_fundrawtransaction.py`
  (`test_change_position`, `test_address_reuse`, `test_external_inputs`,
  `test_preset_inputs_selection`, `test_weight_limits`,
  `test_cannot_cover_fees`)
- `test/functional/wallet_send.py` (manual inputs, change address and type,
  `lock_unspents`, unsafe inputs, external inputs, weight limits)
- `test/functional/wallet_bumpfee.py`
  (`test_bumpfee_with_descendant_fails`, `test_watchonly_psbt`,
  `test_bumpfee_already_spent`, `test_unconfirmed_not_spendable`,
  `test_change_script_match`)
- `test/functional/wallet_balance.py` (`test_balances`)
- `test/functional/wallet_avoidreuse.py`
  (`test_change_remains_change`, `test_getbalances_used`)

## Adjacent pages

- `[[areas/wallet]]`
- `[[areas/rpc-rest-zmq-and-interfaces]]`
- `[[concepts/descriptors]]`
- `[[concepts/fee-estimation]]`
- `[[files/src/wallet/spend.cpp]]`

## Open questions

- `GetBalance()` and the `getbalance(minconf=...)` RPC family currently operate
  on current unspent outputs with depth filtering, not on a separate
  "balance-before-later-spends" model. `test/functional/wallet_balance.py`
  still carries a TODO around spentness depth, so any future wiki page on
  balance semantics should call that limitation out explicitly.
- `CreateTransactionInternal()` still documents a stale-backup risk for fresh
  change keys. It would be useful to verify separately whether current
  descriptor-wallet backup and restore flows fully remove that risk or only
  narrow it.

## Sources consulted

- `src/wallet/spend.cpp`
- `src/wallet/wallet.cpp`
- `src/wallet/receive.cpp`
- `src/wallet/feebumper.cpp`
- `src/wallet/rpc/spend.cpp`
- `src/wallet/transaction.h`
- `src/rpc/rawtransaction_util.cpp`
- `src/wallet/test/spend_tests.cpp`
- `src/wallet/test/psbt_wallet_tests.cpp`
- `src/wallet/test/wallet_tests.cpp`
- `test/functional/wallet_fundrawtransaction.py`
- `test/functional/wallet_send.py`
- `test/functional/wallet_bumpfee.py`
- `test/functional/wallet_balance.py`
- `test/functional/wallet_avoidreuse.py`
