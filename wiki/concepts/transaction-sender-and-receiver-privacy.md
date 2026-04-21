---
kind: concept
title: Transaction Sender and Receiver Privacy
status: active
last_reviewed: 2026-04-21
paths:
  - src/wallet/spend.cpp
  - src/wallet/wallet.cpp
  - src/wallet/rpc/spend.cpp
  - src/wallet/rpc/coins.cpp
  - src/wallet/rpc/addresses.cpp
  - src/wallet/scriptpubkeyman.cpp
  - src/node/transaction.cpp
  - src/net_processing.cpp
tags:
  - wallet
  - privacy
  - relay
  - coin-selection
---

# Transaction Sender and Receiver Privacy

## Summary

Current Bitcoin Core privacy behavior here is wallet-side and relay-side, not consensus-enforced. The wallet tries to reduce some common wallet-origin heuristics, but it does not hide transaction graph structure, amounts, or eventual broadcast origin.

- `sender privacy loss`: input consolidation, address reuse, zero-conf change spending, and repeated rebroadcasts can link multiple sends to the same wallet.
- `receiver privacy loss`: exported PSBTs and wallet RPC responses can reveal descriptor structure, parent descriptors, labels, change status, and BIP32 origin metadata to whoever receives them.
- `operator privacy loss`: wallet-origin transactions are normally submitted to the local mempool and broadcast to all tx-relay peers; timer jitter reduces but does not remove origin-linkage risk.

Facts:

- Automatic change creation in `src/wallet/spend.cpp` (`CreateTransactionInternal`) reserves a fresh internal destination via `src/wallet/wallet.cpp` (`ReserveDestination::GetReservedDestination`). The code comment explicitly says this is done to make change less obvious.
- Change script type selection in `src/wallet/wallet.cpp` (`CWallet::TransactionChangeType`) prefers an explicit `change_type`, otherwise tries to match recipient output types when the wallet has a matching internal script pubkey manager, then falls back to the wallet default.
- `sendtoaddress` and `sendmany` go through `src/wallet/rpc/spend.cpp` (`SendMoney`), which shuffles the recipient vector before calling `CreateTransaction()`. `CreateTransactionInternal()` also randomizes the change position if the caller did not fix it.
- Selected inputs are shuffled by `src/wallet/coinselection.cpp` (`SelectionResult::GetShuffledInputVector`).

Inference:

- Matching change type to recipient type and randomizing input/output placement reduce simple wallet-fingerprint heuristics, but they do not make change identification or wallet clustering impossible.

## Invariants

- Fresh automatic change is internal-only and non-reused by default. `CreateTransactionInternal()` reserves change from an internal script pubkey manager, and only calls `ReserveDestination::KeepDestination()` after transaction creation succeeds. Privacy impact: `sender privacy loss` increases if callers override this with a fixed `change_address` or reuse-address behavior elsewhere.
- Dirty-coin avoidance is opt-in at the wallet level and enforced during coin selection. `src/wallet/walletutil.h` defines `WALLET_FLAG_AVOID_REUSE`; `src/wallet/wallet.cpp` (`SetSpentKeyState`, `IsSpentKey`) marks previously spent wallet destinations; `src/wallet/spend.cpp` (`AvailableCoins`) excludes such outputs unless coin control explicitly disables reuse avoidance. Privacy impact: reduces repeated spending from the same receive address.
- Reuse avoidance also forces address grouping in common send RPCs. `src/wallet/rpc/util.cpp` (`GetAvoidReuseFlag`) and `src/wallet/rpc/spend.cpp` (`sendtoaddress`) set `m_avoid_address_reuse`, and `sendtoaddress` additionally sets `m_avoid_partial_spends` when reuse avoidance is active. `src/wallet/spend.cpp` (`GroupOutputs`) then groups UTXOs by `(scriptPubKey, OutputType)` instead of considering each output separately. Privacy impact: intended to sweep reused destinations into fewer spends and generate clean change, at the cost of larger present-day self-linkage and often higher fees.
- Automatic selection prefers safer and less-linking candidates first. `src/wallet/spend.cpp` (`AutomaticCoinSelection`) first tries confirmed outputs, prefers a single output type through `AttemptSelection()`, and only falls back to zero-conf change or explicitly unsafe inputs when enabled by wallet settings or coin control. Privacy impact: reduces accidental linkage to external unconfirmed transactions by default.
- Wallet rebroadcast is intentionally jittered but still wallet-origin traffic. `src/wallet/wallet.cpp` (`GetDefaultNextResend`, `ShouldResend`, `ResubmitWalletTransactions`) uses a random 12-24 hour resend timer and only periodically re-submits older unconfirmed wallet transactions. `src/net_processing.cpp` (`ReattemptInitialBroadcast`) separately retries initial unbroadcast mempool transactions every 10-15 minutes with randomness. Privacy impact: partial mitigation for `operator privacy loss`, not elimination.
- Wallet-origin broadcasts currently use public tx relay, not private broadcast. `src/wallet/wallet.cpp` (`CommitTransaction`, `SubmitTxMemoryPoolAndRelay`, `MaybeResendWalletTxs`) uses `node::TxBroadcast::MEMPOOL_AND_BROADCAST_TO_ALL`. `src/node/transaction.cpp` (`BroadcastTransaction`) has a private-broadcast mode, but the current wallet commit path does not call it. Privacy impact: wallet sends are normally origin-linkable to the node’s public relay set unless `-walletbroadcast=0` disables submission.

## Important code paths

- Transaction shape and change:
  - `src/wallet/spend.cpp` (`CreateTransactionInternal`) builds recipients in order, inserts change at a random position unless `change_pos` is fixed, computes change viability, and signs the final transaction.
  - `src/wallet/wallet.cpp` (`CWallet::TransactionChangeType`, `ReserveDestination::GetReservedDestination`) chooses change type and allocates internal change destinations.
  - `src/wallet/coinselection.cpp` (`GenerateChangeTarget`) randomizes the preferred minimum change target. Inference: this makes some change/no-change decisions and resulting change sizes less deterministic than a fixed target would.
- Coin selection and reuse boundaries:
  - `src/wallet/spend.cpp` (`AvailableCoins`) filters locked, spent, reused, unsafe, immature, replaced, and depth-ineligible outputs.
  - `src/wallet/spend.cpp` (`GroupOutputs`, `AttemptSelection`, `ChooseSelectionResult`, `AutomaticCoinSelection`) groups by script when APS is active and chooses among BnB, Knapsack, CoinGrinder, and SRD.
  - `src/wallet/wallet.cpp` (`SetSpentKeyState`, `SetAddressPreviouslySpent`) persists dirty-address state in the wallet DB.
  - `src/wallet/rpc/spend.cpp` (`sendall`) explicitly spends the wallet’s eligible UTXO set or a specified subset. Privacy impact: strong `sender privacy loss` because it intentionally links many coins in one spend.
- Relay and rebroadcast linkage:
  - `src/wallet/wallet.cpp` (`postInitProcess`) re-submits unconfirmed wallet transactions to the local mempool on startup with `MEMPOOL_NO_BROADCAST`, specifically avoiding immediate network rebroadcast.
  - `src/wallet/wallet.cpp` (`SubmitTxMemoryPoolAndRelay`, `ResubmitWalletTransactions`) handles normal wallet submission and later rebroadcast.
  - `src/node/transaction.cpp` (`BroadcastTransaction`) either inserts into the mempool and announces to all peers or uses private broadcast for other call sites.
  - `src/rpc/mempool.cpp` (`sendrawtransaction`) switches to `TxBroadcast::NO_MEMPOOL_PRIVATE_BROADCAST` when `-privatebroadcast` is enabled; this is a node/RPC boundary, not the standard wallet commit boundary.
- Local metadata and export surfaces:
  - `src/wallet/transaction.h` (`CWalletTx::mapValue`) stores `"comment"` and `"to"` locally in the wallet DB; `src/wallet/rpc/spend.cpp` (`sendtoaddress`, `sendmany`) populates them. These are not serialized into the Bitcoin transaction.
  - `src/wallet/rpc/coins.cpp` (`listunspent`) can return `label`, `reused`, `desc`, and `parent_descs`.
  - `src/wallet/rpc/addresses.cpp` (`getaddressinfo`) can return `desc`, `parent_desc`, `ischange`, and key-origin metadata.
  - `src/wallet/scriptpubkeyman.cpp` (`DescriptorScriptPubKeyMan::FillPSBT`) fills PSBT inputs and outputs with redeem scripts, witness scripts, and optionally BIP32 derivation/origin data. `walletprocesspsbt` and `walletcreatefundedpsbt` default `bip32derivs=true` in `src/wallet/rpc/spend.cpp`. Privacy impact: `receiver privacy loss` or `sender privacy loss` if PSBTs are shared with parties that should not learn wallet structure or change ownership hints.

## Related tests

- `test/functional/wallet_avoidreuse.py`
- `test/functional/wallet_groups.py`
- `test/functional/wallet_change_address.py`
- `test/functional/wallet_send.py`
- `test/functional/wallet_sendall.py`
- `test/functional/wallet_bumpfee.py`
- `test/functional/mempool_unbroadcast.py`
- `test/functional/wallet_multisig_descriptor_psbt.py`

## Adjacent pages

- `[[areas/wallet]]`
- `[[areas/p2p-and-networking]]`
- `[[concepts/descriptors]]`
- `[[workflows/transaction-acceptance]]`
- `[[files/src/wallet/spend.cpp]]`

## Open questions

- `src/wallet/wallet.cpp` (`ResubmitWalletTransactions`) contains an explicit TODO that the wallet should ideally rebroadcast only transactions that should have mined already; the current broader rebroadcast policy is acknowledged in-tree as privacy-damaging.
- `CreateTransactionInternal()` only randomizes change placement. For higher-level RPCs that do not go through `SendMoney()`, recipient-order privacy depends on caller behavior and any explicit `change_position` choice.

## Sources consulted

- `src/wallet/spend.cpp`
- `src/wallet/coinselection.cpp`
- `src/wallet/wallet.cpp`
- `src/wallet/wallet.h`
- `src/wallet/walletutil.h`
- `src/wallet/transaction.h`
- `src/wallet/rpc/spend.cpp`
- `src/wallet/rpc/coins.cpp`
- `src/wallet/rpc/addresses.cpp`
- `src/wallet/rpc/util.cpp`
- `src/wallet/scriptpubkeyman.cpp`
- `src/node/transaction.cpp`
- `src/node/types.h`
- `src/net_processing.cpp`
- `src/wallet/init.cpp`
- `src/wallet/receive.h`
- `test/functional/wallet_avoidreuse.py`
- `test/functional/wallet_groups.py`
- `test/functional/wallet_change_address.py`
- `test/functional/wallet_send.py`
- `test/functional/wallet_sendall.py`
- `test/functional/wallet_bumpfee.py`
- `test/functional/mempool_unbroadcast.py`
