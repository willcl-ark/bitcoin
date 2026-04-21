---
kind: file
title: src/wallet/load.cpp
status: active
last_reviewed: 2026-04-21
paths:
  - src/wallet/load.cpp
tags:
  - wallet
  - startup
  - loading
  - restore
  - migration
---

# src/wallet/load.cpp

## Role in the System

`src/wallet/load.cpp` is the wallet subsystem's startup coordinator. It
implements the four lifecycle hooks declared in `src/wallet/load.h` and wired
into the node through `wallet::WalletLoaderImpl`, so it is the file that turns
persisted wallet startup settings into live `CWallet` instances during node
init and back into orderly unload during shutdown (`src/wallet/load.cpp`
(`VerifyWallets`, `LoadWallets`, `StartWallets`, `UnloadWallets`);
`src/wallet/load.h`; `src/wallet/interfaces.cpp`
(`wallet::WalletLoaderImpl::verify`, `load`, `start`, `stop`); `src/init.cpp`
(client `verify()`, `load()`, `start()` calls)).

With a fund-safety and startup-integrity lens, this file matters because it
decides which wallet databases are allowed into process memory, when those
wallets may begin mempool synchronization and rebroadcast work, and when
shutdown may release them. It does not own wallet creation, backup restore, or
migration rollback; those flows call helpers in `src/wallet/wallet.cpp`
directly and have their own cleanup logic (`src/wallet/rpc/wallet.cpp`
(`createwallet`, `loadwallet`, `migratewallet`); `src/wallet/rpc/backup.cpp`
(`restorewallet`); `src/wallet/wallet.cpp` (`CreateWallet`, `LoadWallet`,
`RestoreWallet`, `MigrateLegacyToDescriptor`)).

## Important Types and Functions

- `wallet::WalletContext` is the shared state bundle passed through all four
  functions. `VerifyWallets()` and `LoadWallets()` require `context.chain` and
  `context.args`; `StartWallets()` additionally requires `context.scheduler`;
  `UnloadWallets()` uses the wallet list protected by
  `WalletContext::wallets_mutex` (`src/wallet/context.h`
  (`wallet::WalletContext`); `src/wallet/load.cpp`).
- `VerifyWallets(WalletContext&)` is the preflight stage. It canonicalizes
  `-walletdir`, rejects non-existent, non-directory, or relative wallet
  directories, logs DB environment info, and validates each configured startup
  wallet with `MakeWalletDatabase(..., require_existing=true, verify=true)`
  before networking starts (`src/wallet/load.cpp` (`VerifyWallets`);
  `src/wallet/wallet.cpp` (`MakeWalletDatabase`); `src/init.cpp` (startup step
  5)).
- `VerifyWallets()` also contains the backward-compatibility wallet discovery
  path. If no `wallet` setting is present and an unnamed top-level wallet
  exists, it injects `""` into the in-memory `"wallet"` settings list using
  `chain.overwriteRwSetting(..., SettingsAction::SKIP_WRITE)`. This makes the
  default unnamed wallet auto-load without persisting a new startup preference
  (`src/wallet/load.cpp` (`VerifyWallets`);
  `test/functional/wallet_startup.py` (`WalletStartupTest.create_unnamed_wallet`,
  `run_test`)).
- `VerifyWallets()` separates fatal and non-fatal startup conditions. Bad
  `-wallet` value types or hard DB verification errors call `chain.initError()`
  and abort startup. Duplicate startup entries are reduced with a warning,
  missing configured wallets are skipped with a warning, and legacy wallets
  disabled by current DB support are deferred to the load phase so the user
  gets a migration-specific warning instead of a generic verification failure
  (`src/wallet/load.cpp` (`VerifyWallets`);
  `test/functional/wallet_multiwallet.py` (`MultiWalletTest.test_init`);
  `src/wallet/rpc/wallet.cpp` (`listwalletdir`)).
- `LoadWallets(WalletContext&)` is the startup materialization stage. It
  re-reads the `wallet` settings list, opens each database with `verify=false`
  on the assumption that `VerifyWallets()` already did the expensive
  verification, and turns each database into a live wallet via
  `CWallet::LoadExisting()` (`src/wallet/load.cpp` (`LoadWallets`);
  `src/wallet/wallet.cpp` (`CWallet::LoadExisting`); `src/init.cpp` (startup
  step 9)).
- `LoadWallets()` treats load failures as startup failures. Missing wallets are
  skipped, legacy wallets disabled by current DB support become warnings, and
  any unsuccessful `CWallet::LoadExisting()` or caught `std::runtime_error`
  becomes `chain.initError()` and aborts node initialization
  (`src/wallet/load.cpp` (`LoadWallets`)).
- `LoadWallets()` is intentionally narrower than the RPC `loadwallet` path. On
  startup it only calls `NotifyWalletLoaded()` and `AddWallet()` after a
  successful `CWallet::LoadExisting()`. It does not call `postInitProcess()`
  itself, so mempool replay and scheduled rebroadcast are deferred until
  `StartWallets()` (`src/wallet/load.cpp` (`LoadWallets`, `StartWallets`);
  `src/wallet/wallet.cpp` (`NotifyWalletLoaded`, `AddWallet`,
  `CWallet::postInitProcess`)).
- `StartWallets(WalletContext&)` is the post-start hook. It runs
  `CWallet::postInitProcess()` for every already-loaded wallet, then schedules
  `MaybeResendWalletTxs(context)` once per minute. `postInitProcess()` first
  resubmits wallet transactions to the local mempool without peer broadcast,
  then refreshes wallet transaction state from the current mempool; periodic
  resend is the later path that may broadcast (`src/wallet/load.cpp`
  (`StartWallets`); `src/wallet/wallet.cpp` (`CWallet::postInitProcess`,
  `MaybeResendWalletTxs`, `CWallet::ResubmitWalletTransactions`);
  `src/init.cpp` (startup step 13)).
- `UnloadWallets(WalletContext&)` snapshots the currently loaded wallets,
  removes them one by one with `RemoveWallet(..., load_on_start=std::nullopt)`,
  and waits for each wallet's custom deleter path to finish via
  `WaitForDeleteWallet()`. Passing `std::nullopt` is important: node shutdown
  does not rewrite the persistent load-on-startup list (`src/wallet/load.cpp`
  (`UnloadWallets`); `src/wallet/wallet.cpp` (`RemoveWallet`,
  `WaitForDeleteWallet`, `UpdateWalletSetting`)).

## Callers and Dependencies

- `src/init.cpp` uses this file through the generic chain-client lifecycle.
  Wallet construction happens earlier via `wallet::WalletInit::Construct`, but
  actual DB verification, DB load, and post-init work happen later and in
  three distinct init phases: verify before network initialization, load after
  indexes are initialized, and start after RPC warmup is finished
  (`src/wallet/init.cpp` (`wallet::WalletInit::Construct`);
  `src/init.cpp` (wallet-related comments and client `verify()`, `load()`,
  `start()` calls)).
- `compile-time gate`: top-level `ENABLE_WALLET` controls whether wallet
  support is built, and `src/wallet/CMakeLists.txt` includes `load.cpp` in the
  `bitcoin_wallet` target only when wallet support is enabled
  (`CMakeLists.txt` (`ENABLE_WALLET` option); `src/wallet/CMakeLists.txt`).
- `runtime gate`: `wallet::WalletInit::Construct` returns early when
  `-disablewallet` is set, so no wallet loader is registered and none of the
  `load.cpp` lifecycle hooks run (`src/wallet/init.cpp`
  (`wallet::WalletInit::ParameterInteraction`, `wallet::WalletInit::Construct`);
  `test/functional/wallet_disable.py`).
- `VerifyWallets()` and `LoadWallets()` both depend on `ReadDatabaseArgs()`
  plus `MakeWalletDatabase()`, so startup wallet behavior is still affected by
  wallet DB runtime args even though creation and restore are not happening
  here (`src/wallet/load.cpp`; `src/wallet/wallet.cpp`
  (`MakeWalletDatabase`)).
- Startup load eventually depends on `CWallet::LoadExisting()` and
  `CWallet::AttachChain()`. Cross-chain reuse checks, descriptor DB corruption
  checks, rescan decisions, prune and assumeutxo restrictions, and wallet-arg
  validation are downstream load failures surfaced through this file, not
  checks implemented locally in `load.cpp` (`src/wallet/wallet.cpp`
  (`CWallet::LoadWalletArgs`, `CWallet::LoadExisting`, `CWallet::AttachChain`);
  `test/functional/wallet_crosschain.py`;
  `src/wallet/test/walletload_tests.cpp` (`wallet_load_descriptors`)).
- Dynamic `createwallet`, `loadwallet`, and `restorewallet` do not use
  `VerifyWallets()` / `LoadWallets()` / `StartWallets()`. The RPC methods call
  `CreateWallet()`, `LoadWallet()`, and `RestoreWallet()` directly, and those
  helpers perform `NotifyWalletLoaded()`, `AddWallet()`, `postInitProcess()`,
  and `UpdateWalletSetting()` themselves (`src/wallet/rpc/wallet.cpp`
  (`createwallet`, `loadwallet`); `src/wallet/rpc/backup.cpp`
  (`restorewallet`); `src/wallet/wallet.cpp` (`LoadWalletInternal`,
  `CreateWallet`, `RestoreWallet`)).
- Migration has its own load ordering. `MigrateLegacyToDescriptor()` makes a
  legacy backup, performs the migration, then loads the migrated wallets with
  `LoadWallet(..., load_on_start=std::nullopt)` in a fixed order where the main
  spendable wallet is loaded first. If any of those loads fail, it unloads any
  created wallets, deletes only the created DB files and directories, and
  restores the legacy backup with `RestoreWallet(..., load_after_restore=false)`
  (`src/wallet/wallet.cpp` (`MigrateLegacyToDescriptor`)). `load.cpp` is not
  the rollback owner for migration failures.

## Related Tests

- `test/functional/wallet_startup.py` verifies unnamed default-wallet
  auto-discovery and persistent load-on-startup behavior.
- `test/functional/wallet_multiwallet.py` covers `-walletdir` validation,
  duplicate `-wallet` handling, bad-path and symlink rejection, concurrent
  `loadwallet` behavior, load and unload flows, and lock-file behavior.
- `test/functional/feature_filelock.py` checks startup failure when another
  process already holds the wallet DB lock.
- `test/functional/wallet_createwallet.py` covers creation-time flag
  combinations and confirms that dynamic creation and later reload succeed
  through the downstream helpers.
- `test/functional/wallet_backup.py` covers restore failures for invalid
  backups and occupied destinations, cleanup on restore failure, restore on
  pruned nodes, and preservation of non-wallet files in target directories.
- `test/functional/wallet_fast_rescan.py` covers restore-time rescans and the
  block-filter fast path used after restored wallets are loaded.
- `test/functional/wallet_crosschain.py` covers cross-chain load and restore
  rejection.
- `test/functional/wallet_migration.py` covers legacy-wallet discovery
  warnings, backup naming, restore-from-backup success, cleanup after failed
  migration, and rollback when post-migration wallet loading fails.
- `src/wallet/test/wallet_tests.cpp` `CreateWallet` verifies the
  `CWallet::LoadExisting()` / `AttachChain()` notification-ordering invariant
  so blocks and mempool entries are not missed while loading.
- `src/wallet/test/walletload_tests.cpp` `wallet_load_descriptors` covers
  descriptor-load corruption and unknown-descriptor failure paths surfaced
  through `CWallet::LoadExisting()`.

## Notes or Risks

- `fund-safety / wrong-wallet risk`: review changes to `VerifyWallets()`
  carefully. `-walletdir` canonicalization, unnamed-wallet auto-discovery, and
  duplicate handling determine which database files get opened at startup.
  Loading the wrong path or the same wallet twice would threaten wallet-state
  integrity (`src/wallet/load.cpp` (`VerifyWallets`);
  `src/wallet/wallet.cpp` (`MakeWalletDatabase`)).
- `startup-integrity invariant`: verify, load, and start are intentionally
  split. `LoadWallets()` assumes pre-verification already happened and defers
  `postInitProcess()` until `StartWallets()`. If those phases are collapsed or
  reordered, startup could mix path validation, DB mutation, rescans, and
  mempool replay in harder-to-reason-about ways (`src/wallet/load.cpp`
  (`VerifyWallets`, `LoadWallets`, `StartWallets`); `src/init.cpp`).
- `failure boundary`: this file only owns startup admission and startup
  orchestration. Cross-chain rejection, pruned-chain load failures,
  assumeutxo-ordering limits, descriptor corruption, restore cleanup, and
  migration rollback all happen in downstream helpers. Reviewers should trace
  startup failures into `CWallet::LoadExisting()`, `CWallet::AttachChain()`,
  `RestoreWallet()`, and `MigrateLegacyToDescriptor()` instead of assuming
  `load.cpp` is the whole load path (`src/wallet/wallet.cpp`
  (`CWallet::LoadExisting`, `CWallet::AttachChain`, `RestoreWallet`,
  `MigrateLegacyToDescriptor`)).
- `persistence boundary`: startup discovery of the unnamed wallet uses
  `overwriteRwSetting(..., SKIP_WRITE)` and shutdown unload uses
  `RemoveWallet(..., load_on_start=std::nullopt)`, so neither action should
  rewrite the persistent startup list. Persistent load-on-startup mutations
  belong to the dynamic create, load, restore, and unload helpers in
  `src/wallet/wallet.cpp` and the RPC surfaces that pass `load_on_startup`
  (`src/wallet/load.cpp` (`VerifyWallets`, `UnloadWallets`);
  `src/wallet/wallet.cpp` (`UpdateWalletSetting`, `LoadWalletInternal`,
  `CreateWallet`, `RemoveWallet`, `RestoreWallet`);
  `src/wallet/rpc/wallet.cpp`; `src/wallet/rpc/backup.cpp`).
- `privacy / rebroadcast timing`: `StartWallets()` schedules
  `MaybeResendWalletTxs()` only after startup is otherwise complete, and
  `postInitProcess()` initially uses `MEMPOOL_NO_BROADCAST`. Changes here can
  affect whether locally created transactions are rediscovered quietly,
  broadcast too early, or not rebroadcast at all (`src/wallet/load.cpp`
  (`StartWallets`); `src/wallet/wallet.cpp` (`CWallet::postInitProcess`,
  `MaybeResendWalletTxs`)).
- `legacy-wallet operator path`: startup intentionally skips
  `FAILED_LEGACY_DISABLED` wallets instead of hard-aborting the node. The
  operator signal is a warning plus the separate migration flow, not automatic
  conversion (`src/wallet/load.cpp` (`VerifyWallets`, `LoadWallets`);
  `src/wallet/rpc/wallet.cpp` (`listwalletdir`, `migratewallet`)).

## Sources Consulted

- `src/wallet/load.cpp`
- `src/wallet/load.h`
- `src/wallet/context.h`
- `src/wallet/interfaces.cpp`
- `src/wallet/init.cpp`
- `src/wallet/wallet.cpp`
- `src/wallet/rpc/wallet.cpp`
- `src/wallet/rpc/backup.cpp`
- `src/init.cpp`
- `src/wallet/CMakeLists.txt`
- `CMakeLists.txt`
- `src/wallet/test/wallet_tests.cpp`
- `src/wallet/test/walletload_tests.cpp`
- `test/functional/wallet_startup.py`
- `test/functional/wallet_multiwallet.py`
- `test/functional/feature_filelock.py`
- `test/functional/wallet_createwallet.py`
- `test/functional/wallet_backup.py`
- `test/functional/wallet_fast_rescan.py`
- `test/functional/wallet_crosschain.py`
- `test/functional/wallet_migration.py`
