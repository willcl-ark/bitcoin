---
kind: file
title: src/wallet/wallet.cpp
status: active
last_reviewed: 2026-04-21
paths:
  - src/wallet/wallet.cpp
tags:
  - wallet
  - wallet-lifecycle
  - rescans
  - encryption
  - migration
---

# src/wallet/wallet.cpp

## Role in the System

`src/wallet/wallet.cpp` is the main implementation file for `wallet::CWallet`.
It is the wallet subsystem's stateful control layer: it loads and unloads
wallets, attaches them to chain notifications, maintains wallet transaction
state, manages address/keypool allocation, handles encryption state, persists
critical metadata, and performs legacy-to-descriptor migration
(`src/wallet/wallet.cpp` (`AddWallet`, `RemoveWallet`, `WaitForDeleteWallet`,
`CWallet::CreateNew`, `CWallet::LoadExisting`, `CWallet::AttachChain`,
`CWallet::CommitTransaction`, `CWallet::EncryptWallet`,
`MigrateLegacyToDescriptor`)).

This file is not the wallet's transaction-construction engine. Coin selection
and transaction assembly live primarily in `src/wallet/spend.cpp`; `wallet.cpp`
owns the persisted wallet view of those transactions after creation, relay,
confirmation, conflict, disconnection, reload, backup, or migration
(`src/wallet/wallet.cpp` (`CWallet::CommitTransaction`,
`CWallet::SubmitTxMemoryPoolAndRelay`, `CWallet::SyncTransaction`,
`CWallet::blockConnected`, `CWallet::blockDisconnected`)).

## Important Types and Functions

- `CWallet` is the central owner of persisted wallet state, including
  `mapWallet`, `mapTxSpends`, the address book, `m_txos`, encryption keys,
  active `ScriptPubKeyMan` instances, and best-block tracking
  (`src/wallet/wallet.cpp` (`CWallet::LoadToWallet`,
  `CWallet::AddToSpends`, `CWallet::RefreshTXOsFromTx`,
  `CWallet::WriteBestBlock`)).
- Wallet lifetime and registration are handled by the free functions
  `AddWallet()`, `RemoveWallet()`, `NotifyWalletLoaded()`,
  `WaitForDeleteWallet()`, and the custom deleter `FlushAndDeleteWallet()`.
  These are the unload boundary that keeps upper layers from using a wallet
  after validation callbacks and shared pointers are released
  (`src/wallet/wallet.cpp` (`FlushAndDeleteWallet`, `WaitForDeleteWallet`,
  `AddWallet`, `RemoveWallet`, `NotifyWalletLoaded`,
  `CWallet::DisconnectChainNotifications`)).
- Load and startup wiring live in `LoadWalletInternal()`,
  `CWallet::LoadWalletArgs()`, `CWallet::CreateNew()`,
  `CWallet::LoadExisting()`, `CWallet::AttachChain()`, and
  `CWallet::postInitProcess()`. These functions validate wallet paths and fee
  args, reject unsafe chain reuse, restore the wallet's best-block position,
  register chain notifications before rescans, and resubmit unconfirmed wallet
  transactions into the local mempool on startup without immediate peer relay
  (`src/wallet/wallet.cpp` (`GetWalletPath`, `MakeWalletDatabase`,
  `CWallet::LoadWalletArgs`, `CWallet::CreateNew`, `CWallet::LoadExisting`,
  `CWallet::AttachChain`, `CWallet::postInitProcess`)).
- Chain synchronization and rescans are implemented by
  `CWallet::RescanFromTime()`, `CWallet::ScanForWalletTransactions()`,
  `FastWalletRescanFilter`, `CWallet::BlockUntilSyncedToCurrentChain()`, and
  the validation callbacks `transactionAddedToMempool()`,
  `transactionRemovedFromMempool()`, `blockConnected()`,
  `blockDisconnected()`, and `updatedBlockTip()`
  (`src/wallet/wallet.cpp` (`FastWalletRescanFilter`,
  `CWallet::RescanFromTime`, `CWallet::ScanForWalletTransactions`,
  `CWallet::transactionAddedToMempool`,
  `CWallet::transactionRemovedFromMempool`, `CWallet::blockConnected`,
  `CWallet::blockDisconnected`, `CWallet::BlockUntilSyncedToCurrentChain`)).
- Wallet transaction insertion and state transitions are centralized in
  `CWallet::LoadToWallet()`, `CWallet::AddToWalletIfInvolvingMe()`,
  `CWallet::SyncTransaction()`, `CWallet::AbandonTransaction()`,
  `CWallet::MarkConflicted()`, `CWallet::RecursiveUpdateTxState()`,
  `CWallet::CommitTransaction()`, `CWallet::SubmitTxMemoryPoolAndRelay()`,
  `CWallet::ResubmitWalletTransactions()`, and `CWallet::RemoveTxs()`
  (`src/wallet/wallet.cpp` (`CWallet::LoadToWallet`,
  `CWallet::AddToWalletIfInvolvingMe`, `CWallet::SyncTransaction`,
  `CWallet::AbandonTransaction`, `CWallet::MarkConflicted`,
  `CWallet::RecursiveUpdateTxState`, `CWallet::CommitTransaction`,
  `CWallet::SubmitTxMemoryPoolAndRelay`,
  `CWallet::ResubmitWalletTransactions`, `CWallet::RemoveTxs`)).
- Address and keypool management are implemented here through
  `CWallet::TopUpKeyPool()`, `CWallet::GetNewDestination()`,
  `CWallet::GetNewChangeDestination()`, `ReserveDestination`,
  `CWallet::LockCoin()`, `CWallet::UnlockCoin()`, and address-book helpers.
  `ReserveDestination` is RAII-backed: if `KeepDestination()` is not called,
  the reserved destination is returned to the originating `ScriptPubKeyMan`
  (`src/wallet/wallet.cpp` (`CWallet::TopUpKeyPool`,
  `CWallet::GetNewDestination`, `CWallet::GetNewChangeDestination`,
  `ReserveDestination::GetReservedDestination`,
  `ReserveDestination::KeepDestination`,
  `ReserveDestination::ReturnDestination`, `CWallet::LockCoin`,
  `CWallet::UnlockCoin`, `CWallet::SetAddressBookWithDB`)).
- Encryption state is enforced here via `EncryptMasterKey()`,
  `DecryptMasterKey()`, `CWallet::EncryptWallet()`, both `CWallet::Unlock()`
  overloads, `CWallet::ChangeWalletPassphrase()`, and `CWallet::Lock()`. The
  file also upgrades descriptor caches after unlocking and cleanses
  `vMasterKey` on lock (`src/wallet/wallet.cpp` (`EncryptMasterKey`,
  `DecryptMasterKey`, `CWallet::EncryptWallet`, `CWallet::Unlock`,
  `CWallet::ChangeWalletPassphrase`, `CWallet::Lock`,
  `CWallet::UpgradeDescriptorCache`)).
- Script ownership and signing-provider routing are coordinated through
  `CWallet::GetActiveScriptPubKeyMans()`, `CWallet::GetScriptPubKeyMan()`,
  `CWallet::GetScriptPubKeyMans()`, `CWallet::ConnectScriptPubKeyManNotifiers()`,
  `CWallet::GetSolvingProvider()`, and descriptor setup helpers
  (`src/wallet/wallet.cpp` (`CWallet::GetActiveScriptPubKeyMans`,
  `CWallet::GetScriptPubKeyMan`, `CWallet::GetScriptPubKeyMans`,
  `CWallet::ConnectScriptPubKeyManNotifiers`,
  `CWallet::GetSolvingProvider`, `CWallet::SetupDescriptorScriptPubKeyMans`,
  `CWallet::AddWalletDescriptor`)).
- Backup and migration are handled by `CWallet::BackupWallet()`,
  `CWallet::MigrateToSQLite()`, `CWallet::GetDescriptorsForLegacy()`,
  `CWallet::ApplyMigrationData()`, `DoMigration()`, and both
  `MigrateLegacyToDescriptor()` overloads. This code makes a backup first,
  converts storage, splits migrated data into spendable/watch-only/solvable
  wallets when needed, and attempts rollback plus backup restore on failure
  (`src/wallet/wallet.cpp` (`CWallet::BackupWallet`,
  `CWallet::MigrateToSQLite`, `CWallet::GetDescriptorsForLegacy`,
  `CWallet::ApplyMigrationData`, `DoMigration`,
  `MigrateLegacyToDescriptor`)).

## Callers and Dependencies

- The exported wallet lifecycle functions declared in `src/wallet/wallet.h`
  are used by wallet init/load/unload flows and by higher-level RPC and GUI
  surfaces that work through wallet interfaces rather than constructing wallets
  directly (`src/wallet/wallet.h` (`LoadWallet`, `CreateWallet`,
  `RestoreWallet`, `HandleLoadWallet`, `MaybeResendWalletTxs`)).
- `wallet.cpp` depends heavily on `interfaces::Chain` for wallet settings,
  block access, mempool queries, relay, verification progress, shutdown checks,
  and notification registration (`src/wallet/wallet.cpp`
  (`AddWalletSetting`, `UpdateWalletSetting`, `CWallet::AttachChain`,
  `CWallet::ScanForWalletTransactions`,
  `CWallet::SubmitTxMemoryPoolAndRelay`,
  `CWallet::BlockUntilSyncedToCurrentChain`)).
- Persistent correctness depends on `WalletDatabase`, `WalletBatch`, and
  database transactions. Many operations intentionally hard-fail rather than
  continue with mixed in-memory/on-disk state
  (`src/wallet/wallet.cpp` (`CWallet::EncryptWallet`,
  `CWallet::CommitTransaction`, `CWallet::PopulateWalletFromDB`,
  `CWallet::RemoveTxs`, `CWallet::MigrateToSQLite`)).
- The file delegates key ownership, descriptor expansion, encryption checks,
  and address generation to `ScriptPubKeyMan` subclasses, especially
  `DescriptorScriptPubKeyMan`, `LegacyDataSPKM`, and
  `ExternalSignerScriptPubKeyMan`
  (`src/wallet/wallet.cpp` (`CWallet::GetAllScriptPubKeyMans`,
  `CWallet::ConnectScriptPubKeyManNotifiers`,
  `CWallet::SetupLegacyScriptPubKeyMan`,
  `CWallet::SetupDescriptorScriptPubKeyMans`,
  `CWallet::DisplayAddress`)).
- Concurrency is explicit. `cs_wallet` protects core wallet state,
  `m_relock_mutex` serializes relock/passphrase transitions, and unload waits on
  `g_wallet_release_mutex` plus validation-queue draining so wallet destruction
  does not race pending callbacks (`src/wallet/wallet.cpp` (`CWallet::Lock`,
  `CWallet::ChangeWalletPassphrase`, `WaitForDeleteWallet`,
  `CWallet::DisconnectChainNotifications`,
  `CWallet::BlockUntilSyncedToCurrentChain`)).

## Related Tests

- `src/wallet/test/wallet_tests.cpp` `scan_for_wallet_transactions` exercises
  `CWallet::ScanForWalletTransactions()` success, reorg, and empty-scan
  behavior.
- `src/wallet/test/wallet_tests.cpp` `CreateWallet` checks the
  `CWallet::CreateNew()` / `CWallet::AttachChain()` notification ordering
  invariant so block and mempool transactions are not missed during load.
- `src/wallet/test/wallet_tests.cpp` `CreateWalletWithoutChain` covers wallet
  creation in a local context and `WaitForDeleteWallet()`.
- `src/wallet/test/wallet_tests.cpp` `RemoveTxs` covers `CWallet::RemoveTxs()`
  state cleanup.
- `src/wallet/test/wallet_tests.cpp` `wallet_disableprivkeys` and
  `ListCoinsTest` cover destination generation and coin-locking behavior tied
  to `GetNewDestination()` and `LockCoin()`.
- `src/wallet/test/walletload_tests.cpp` `wallet_load_descriptors` covers
  `CWallet::PopulateWalletFromDB()` error paths for unknown descriptors and
  descriptor-ID corruption.
- `src/wallet/test/wallet_crypto_tests.cpp` `passphrase`, `encrypt`, and
  `decrypt` exercise the `CCrypter` behavior used by `EncryptMasterKey()` and
  `DecryptMasterKey()`.
- `test/functional/wallet_encryption.py` `WalletEncryptionTest.run_test()`
  covers `encryptwallet`, `walletpassphrase`, `walletpassphrasechange`,
  `walletlock`, timeout bounds, and the no-private-keys encryption guard that
  maps back to `CWallet::EncryptWallet()`, `CWallet::Unlock()`,
  `CWallet::ChangeWalletPassphrase()`, and `CWallet::Lock()`.
- `test/functional/wallet_keypool.py` `KeyPoolTest.run_test()` covers keypool
  refill/exhaustion, change-address reservation, encrypted-wallet keypool
  behavior, and failure modes when no change address can be generated.
- `test/functional/wallet_resendwallettransactions.py`
  `ResendWalletTransactionsTest.run_test()` covers `CWallet::ShouldResend()` /
  `CWallet::ResubmitWalletTransactions()`, including randomized rebroadcast
  timing and parent-before-child resubmission.
- `test/functional/wallet_backup.py` `WalletBackupTest.run_test()`,
  `test_restore_existent_dir()`, and `test_pruned_wallet_backup()` cover
  `CWallet::BackupWallet()`, restore/load behavior, and the requirement that
  best-block metadata be current enough to reload close to a prune boundary.
- `test/functional/wallet_createwallet.py` `CreateWalletTest.run_test()` covers
  creation-time flag combinations and verifies encrypted and blank wallet
  behaviors that are established in `CWallet::CreateNew()` and
  `CWallet::LoadWalletArgs()`.
- `test/functional/wallet_multiwallet.py` `MultiWalletTest` exercises
  load/unload, invalid wallet paths, backup and restore, and lock-file
  interactions that depend on the load/remove helpers in this file.
- `test/functional/wallet_migration.py`
  `WalletMigrationTest.migrate_and_get_rpc()` and
  `WalletMigrationTest.run_test()` cover `MigrateLegacyToDescriptor()`, backup
  naming, SQLite conversion, rollback behavior, descriptor migration, and
  watch-only/solvable wallet creation.

## Notes or Risks

- `crash / abort boundary`: several paths intentionally stop the process rather
  than continue with partially updated wallet state. `CWallet::EncryptWallet()`
  asserts if key encryption or the enclosing DB commit fails after keys were
  mutated in memory. `CWallet::MigrateToSQLite()` asserts if it cannot write the
  replacement SQLite DB after deleting the old DB file. `CWallet::CommitTransaction()`
  and `CWallet::AddToWalletIfInvolvingMe()` throw on wallet DB write failure
  (`src/wallet/wallet.cpp` (`CWallet::EncryptWallet`,
  `CWallet::MigrateToSQLite`, `CWallet::CommitTransaction`,
  `CWallet::AddToWalletIfInvolvingMe`)).
- `offline / load failure`: `CWallet::AttachChain()` rejects wallet reuse across
  chains unless `-walletcrosschain` is set, and it fails load when required
  blocks are unavailable due to pruning or assumeutxo ordering limits.
  `CWallet::PopulateWalletFromDB()` also treats unknown descriptors, unexpected
  legacy entries in descriptor wallets, and corruption as hard load failures
  (`src/wallet/wallet.cpp` (`CWallet::AttachChain`,
  `CWallet::PopulateWalletFromDB`)).
- `fund-loss / wrong-spendability risk`: wallet accounting depends on consistent
  transaction-state transitions across mempool events, confirmed blocks,
  disconnects, conflicts, and abandon/rebroadcast flows. If
  `AddToWalletIfInvolvingMe()`, `MarkConflicted()`,
  `RecursiveUpdateTxState()`, `blockConnected()`,
  `blockDisconnected()`, or `transactionRemovedFromMempool()` are wrong, the
  wallet can misreport balance or treat spent/conflicted coins as available
  (`src/wallet/wallet.cpp` (`CWallet::AddToWalletIfInvolvingMe`,
  `CWallet::MarkConflicted`, `CWallet::RecursiveUpdateTxState`,
  `CWallet::blockConnected`, `CWallet::blockDisconnected`,
  `CWallet::transactionRemovedFromMempool`)).
- `fund-loss containment on send`: `CWallet::CommitTransaction()` persists the
  transaction before relay so the wallet keeps a durable record of the outgoing
  spend and its change even if broadcast fails. That avoids silently losing the
  wallet-side view of a created transaction, but it also means relay failure
  leaves an unconfirmed wallet transaction that later resend logic must manage
  correctly (`src/wallet/wallet.cpp` (`CWallet::CommitTransaction`,
  `CWallet::SubmitTxMemoryPoolAndRelay`,
  `CWallet::ResubmitWalletTransactions`)).
- `privacy leakage`: rebroadcast timing is deliberately randomized and disabled
  when the chain is not ready to broadcast. Startup resubmission uses
  `MEMPOOL_NO_BROADCAST` specifically to repopulate the local mempool without
  immediately announcing wallet state to peers. Breaking `ShouldResend()` or
  the startup path would make wallet-origin transactions more linkable
  (`src/wallet/wallet.cpp` (`CWallet::ShouldResend`,
  `CWallet::ResubmitWalletTransactions`, `CWallet::postInitProcess`)).
- `address reuse / change safety`: `ReserveDestination` must return unused
  change destinations unless `KeepDestination()` is called. `GetNewDestination()`
  also backfills the address book for newly discovered receive addresses during
  wallet sync. Bugs here can cause change-address failure, accidental reuse, or
  incomplete receive accounting (`src/wallet/wallet.cpp`
  (`ReserveDestination::GetReservedDestination`,
  `ReserveDestination::KeepDestination`,
  `ReserveDestination::ReturnDestination`, `CWallet::GetNewDestination`,
  `CWallet::GetNewChangeDestination`, `CWallet::AddToWalletIfInvolvingMe`)).
- `resource / startup cost`: `CWallet::ScanForWalletTransactions()` can do a
  full historic rescan. It tries to bound work with block filters, progress
  checkpoints, abort/shutdown checks, and saved best-block locators, but bad
  assumptions here can still mean long startup stalls or rescans that cannot
  complete on pruned nodes (`src/wallet/wallet.cpp`
  (`FastWalletRescanFilter`, `CWallet::ScanForWalletTransactions`,
  `CWallet::RescanFromTime`, `CWallet::WriteBestBlock`)).
- `migration safety`: `MigrateLegacyToDescriptor()` makes a backup before DB
  conversion, and on later failure it unloads any created wallets, removes only
  newly created DB files/directories, and restores from the backup.
  `ApplyMigrationData()` fails if transactions or address-book entries cannot be
  assigned to one of the migrated wallets, which is the right bias for
  fund-safety and accounting correctness (`src/wallet/wallet.cpp`
  (`MigrateLegacyToDescriptor`, `CWallet::BackupWallet`,
  `CWallet::ApplyMigrationData`, `CWallet::RemoveTxs`)).
- `lifetime / concurrency`: unload safety depends on draining validation
  callbacks before destruction. `CWallet::DisconnectChainNotifications()`
  disconnects then waits for outstanding notifications, and
  `WaitForDeleteWallet()` blocks until the custom deleter runs. If callers
  bypass this pattern, stale callbacks could hit freed wallet state
  (`src/wallet/wallet.cpp` (`CWallet::DisconnectChainNotifications`,
  `WaitForDeleteWallet`, `FlushAndDeleteWallet`)).
- `fee-safety guardrails`: `CWallet::LoadWalletArgs()` validates fee-related
  wallet args, warns on abnormally high settings, and rejects `-maxtxfee`
  values below the relay minimum to avoid obviously stuck self-created
  transactions. These are configuration-time guardrails, not a substitute for
  spend-path correctness (`src/wallet/wallet.cpp` (`CWallet::LoadWalletArgs`)).

## Sources Consulted

- `src/wallet/wallet.cpp`
- `src/wallet/wallet.h`
- `src/wallet/test/wallet_tests.cpp`
- `src/wallet/test/walletload_tests.cpp`
- `src/wallet/test/wallet_crypto_tests.cpp`
- `test/functional/wallet_encryption.py`
- `test/functional/wallet_keypool.py`
- `test/functional/wallet_resendwallettransactions.py`
- `test/functional/wallet_backup.py`
- `test/functional/wallet_createwallet.py`
- `test/functional/wallet_multiwallet.py`
- `test/functional/wallet_migration.py`
