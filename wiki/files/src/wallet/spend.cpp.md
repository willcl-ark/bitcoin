---
kind: file
title: src/wallet/spend.cpp
status: active
last_reviewed: 2026-04-21
paths:
  - src/wallet/spend.cpp
tags:
  - wallet
  - spending
  - coin-selection
---

# src/wallet/spend.cpp

## Role in the System

`src/wallet/spend.cpp` is the wallet transaction-construction engine. It owns
available-coin filtering, input fetching, automatic coin selection, change
decision logic, fee calculation integration, and the main `CreateTransaction`
path used by wallet spending RPCs and wallet-side transaction creation flows.

## Important Types and Functions

- `AvailableCoins()` filters the wallet's spendable coins for transaction
  creation.
- `FetchSelectedInputs()` resolves explicitly selected inputs.
- `AttemptSelection()` and `ChooseSelectionResult()` run coin-selection logic
  over grouped outputs.
- `AutomaticCoinSelection()` chooses inputs when the caller does not fully
  specify them.
- `CreateTransactionInternal()` is the main assembly loop for recipients,
  selected coins, change, weight, and fee iteration.
- `CreateTransaction()` is the exported wrapper used by higher-level wallet
  code.

## Callers and Dependencies

- Called by wallet spending and fee-bumping flows and by wallet RPC entry
  points that create transactions or PSBTs.
- Depends on wallet state and locking, coin-selection helpers from
  `src/wallet/spend.h`, fee estimation via the chain interface, and signing
  behavior controlled by wallet/key availability.
- Interacts with descriptor solvability, change-type selection, and fee policy
  from the broader wallet subsystem.

## Related Tests

- `src/wallet/test/spend_tests.cpp`
- `src/wallet/test/coinselector_tests.cpp`
- `src/wallet/test/fuzz/spend.cpp`
- `test/functional/wallet_sendall.py`
- `test/functional/wallet_bumpfee.py`
- `test/functional/wallet_spend_unconfirmed.py`
- `test/functional/wallet_anchor.py`

## Notes or Risks

- Critical categories:
  - `fund-loss`: bugs here can select the wrong coins, compute the wrong fee,
    mis-handle change, or fail to respect wallet constraints when creating
    spends.
  - `sender/receiver privacy`: coin selection, change handling, and output
    grouping directly affect transaction linkability.
  - `resource`: pathological selection loops or oversized candidate evaluation
    can degrade wallet responsiveness.
- This file is a direct fund-safety boundary because it converts wallet
  balances and intent into a specific transaction shape.
- The privacy implications are not just policy-level. Selection strategy,
  change type, and solvability constraints can materially change what an
  observer learns from the resulting transaction.

## Sources Consulted

- `src/wallet/spend.h`
- `src/wallet/spend.cpp`
- `src/wallet/test/spend_tests.cpp`
- `src/wallet/test/coinselector_tests.cpp`
- `src/wallet/test/fuzz/spend.cpp`
- `test/functional/wallet_sendall.py`
- `test/functional/wallet_bumpfee.py`
- `test/functional/wallet_spend_unconfirmed.py`
- `test/functional/wallet_anchor.py`
