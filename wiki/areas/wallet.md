---
kind: area
title: Wallet
status: active
last_reviewed: 2026-04-20
paths:
  - src/wallet/
  - src/interfaces/wallet.h
  - src/bitcoin-wallet.cpp
tags:
  - wallet
  - descriptors
  - rpc
---

# Wallet

## Summary

The wallet subsystem owns local wallet databases, key and script managers, wallet transaction state, and wallet-specific RPCs. It is connected to the rest of the node through `interfaces::WalletLoader` and `interfaces::Wallet` in `src/interfaces/wallet.h`, so node, GUI, and wallet code do not need direct cross-library calls for most operations.

## Responsibilities and Invariants

- Startup wiring lives in `src/wallet/init.cpp` (`wallet::WalletInit`). `AddWalletOptions()` registers wallet runtime options, `ParameterInteraction()` makes `-disablewallet` override `-wallet=...`, and `Construct()` skips wallet setup entirely when `-disablewallet` is set.
- The main startup sequence is `VerifyWallets()` -> `LoadWallets()` -> `StartWallets()` in `src/wallet/load.cpp`, wrapped by `wallet::WalletLoaderImpl` in `src/wallet/interfaces.cpp`.
- `VerifyWallets()` canonicalizes `-walletdir`, rejects non-absolute or non-directory wallet dirs, checks configured `-wallet` entries, and deduplicates wallet paths by absolute path before load.
- `GetWalletDir()` in `src/wallet/walletutil.cpp` uses `-walletdir` when present; otherwise it prefers `<datadir>/wallets` if that directory already exists, and falls back to the network datadir.
- `WalletLoaderImpl::registerRpcs()` registers each command from `GetWalletRPCCommands()` and injects a `WalletContext` into the request before dispatch (`src/wallet/interfaces.cpp`).
- `StartWallets()` runs `CWallet::postInitProcess()` for loaded wallets and schedules `MaybeResendWalletTxs()` once per minute; `UnloadWallets()` removes wallets and waits for deletion (`src/wallet/load.cpp`).
- Current creation paths are descriptor-only. `CreateWallet()` and descriptor-migration helpers force `DatabaseFormat::SQLITE` in `src/wallet/wallet.cpp`. Legacy Berkeley DB wallets are only reopened read-only for migration via `DatabaseFormat::BERKELEY_RO` and `MigrateLegacyToDescriptor()`.
- The standalone `bitcoin-wallet` tool in `src/bitcoin-wallet.cpp` is offline-oriented and exposes `info`, `create`, `dump`, and `createfromdump` commands against wallet files without starting the full node.

## Important Code Paths

- Initialization and option registration: `src/wallet/init.cpp` (`wallet::WalletInit::AddWalletOptions`, `ParameterInteraction`, `Construct`)
- Loader boundary used by node and GUI: `src/wallet/interfaces.cpp` (`wallet::WalletLoaderImpl`, `interfaces::MakeWalletLoader`, `interfaces::MakeWallet`)
- Startup verification and load/unload: `src/wallet/load.cpp` (`VerifyWallets`, `LoadWallets`, `StartWallets`, `UnloadWallets`)
- Wallet state and database operations: `src/wallet/wallet.cpp` (`LoadWallet`, `CreateWallet`, `RestoreWallet`, `MigrateLegacyToDescriptor`, `CWallet::LoadExisting`, `CWallet::CreateNew`)
- Wallet directory and default descriptor generation: `src/wallet/walletutil.cpp` (`GetWalletDir`, `GenerateWalletDescriptor`)

## Compile and Runtime Gates

- Compile-time wallet boundary: `interfaces::MakeWalletLoader()` is documented in `src/interfaces/wallet.h` as unavailable when `ENABLE_WALLET` is false.
- Runtime disable switch: `-disablewallet` in `src/wallet/init.cpp`
- Runtime wallet selection and storage: `-wallet=<path>`, `-walletdir=<dir>` in `src/wallet/init.cpp`
- Wallet behavior flags: `-walletbroadcast`, `-walletrbf`, `-walletnotify`, `-avoidpartialspends`, `-spendzeroconfchange`, `-txconfirmtarget`, and related fee knobs in `src/wallet/init.cpp`
- External signer integration is conditional on `ENABLE_EXTERNAL_SIGNER` for `-signer=<cmd>` in `src/wallet/init.cpp`

## Related Tests

- Unit and wallet-specific tests: `src/wallet/test/init_tests.cpp`, `src/wallet/test/walletload_tests.cpp`, `src/wallet/test/wallet_rpc_tests.cpp`, `src/wallet/test/wallet_tests.cpp`, `src/wallet/test/psbt_wallet_tests.cpp`
- Functional coverage: `test/functional/wallet_createwallet.py`, `test/functional/wallet_multiwallet.py`, `test/functional/wallet_disable.py`, `test/functional/wallet_startup.py`, `test/functional/wallet_migration.py`, `test/functional/tool_wallet.py`

## Adjacent Pages

- `[[areas/rpc-rest-zmq-and-interfaces]]`
- `[[areas/common-utils-and-configuration]]`
- `[[areas/testing]]`
- `[[concepts/descriptors]]`
- `[[concepts/transaction-sender-and-receiver-privacy]]`
- `[[concepts/wallet-fund-safety]]`
- `[[files/src/wallet/spend.cpp]]`
- `[[files/src/wallet/wallet.cpp]]`
- `[[investigations/critical-codepaths-priority-map]]`

## Sources Consulted

- `src/wallet/init.cpp`
- `src/wallet/load.cpp`
- `src/wallet/interfaces.cpp`
- `src/wallet/wallet.cpp`
- `src/wallet/walletutil.cpp`
- `src/interfaces/wallet.h`
- `src/bitcoin-wallet.cpp`
- `doc/managing-wallets.md`
