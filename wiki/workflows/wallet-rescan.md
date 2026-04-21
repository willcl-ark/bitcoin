---
kind: workflow
title: Wallet Rescan
status: active
last_reviewed: 2026-04-21
paths:
  - src/wallet/wallet.cpp
  - src/wallet/wallet.h
  - src/wallet/rpc/backup.cpp
  - src/wallet/rpc/transactions.cpp
  - src/wallet/scriptpubkeyman.cpp
  - doc/managing-wallets.md
tags:
  - wallet
  - rescan
  - restore
  - descriptors
---

# Wallet Rescan

## Summary

Wallet rescans rebuild wallet-local transaction and coin state from the active
chain and, for unbounded scans, the current mempool. In the current tree this
workflow is centered on `src/wallet/wallet.cpp`
(`CWallet::AttachChain`, `CWallet::RescanFromTime`,
`CWallet::ScanForWalletTransactions`) with RPC entry points in
`src/wallet/rpc/backup.cpp` (`importdescriptors`, `restorewallet`) and
`src/wallet/rpc/transactions.cpp` (`rescanblockchain`, `abortrescan`).

Current rescans are descriptor-centric. The fast path in
`src/wallet/wallet.cpp` (`FastWalletRescanFilter`) builds a basic block-filter
set from `DescriptorScriptPubKeyMan` scriptPubKeys, and `importdescriptors`
adds or updates wallet descriptors through `src/wallet/wallet.cpp`
(`CWallet::AddWalletDescriptor`).

## Entry Points

- Startup and restore use the same load path:
  `src/wallet/wallet.cpp` (`CWallet::LoadExisting` ->
  `CWallet::AttachChain`). `src/wallet/rpc/backup.cpp` (`restorewallet`) only
  copies the backup into the wallet directory, then calls `LoadWallet()`, so
  restore-time rescans are just normal wallet-load rescans.
- Descriptor imports use `src/wallet/rpc/backup.cpp`
  (`importdescriptors`). After parsing and storing the descriptors, the RPC
  calls `CWallet::RescanFromTime()` from the earliest requested timestamp.
- Manual rescans use `src/wallet/rpc/transactions.cpp`
  (`rescanblockchain`), which chooses a start block by height and calls
  `CWallet::ScanForWalletTransactions()` directly.
- Aborts use `src/wallet/rpc/transactions.cpp` (`abortrescan`), which sets
  `src/wallet/wallet.h` (`CWallet::AbortRescan`).
- Pruned-wallet users who cannot rescan can fall back to
  `src/wallet/rpc/backup.cpp` (`importprunedfunds`), which imports specific
  confirmed transactions without scanning the chain.

## Workflow

### 1. Determine the scan start point

- On load, `src/wallet/wallet.cpp` (`CWallet::LoadExisting`) treats
  `DBErrors::NEED_RESCAN` from `PopulateWalletFromDB()` as a forced rescan. In
  that case `CWallet::AttachChain()` starts from height 0.
- Otherwise `CWallet::AttachChain()` reads the wallet's saved best-block
  locator and uses `interfaces::Chain::findLocatorFork()` to find the last
  height known to both wallet and active chain. This is the first guard
  against missed reorg notifications or stale wallet state.
- `CWallet::AttachChain()` then narrows the start height using the wallet
  birthday (`m_birth_time`) and
  `interfaces::Chain::findFirstBlockWithTimeAndHeight(time_first_key - TIMESTAMP_WINDOW, ...)`.
  This skips blocks older than the earliest relevant key or descriptor time
  while still keeping the two-hour timestamp grace window.
- `src/wallet/wallet.cpp` (`CWallet::RescanFromTime`) does the same
  timestamp-based lookup for `importdescriptors`: it finds the first block at
  or after `startTime - TIMESTAMP_WINDOW`, then scans forward from there.
- `src/wallet/rpc/transactions.cpp` (`rescanblockchain`) does not use wallet
  birthdays. It validates explicit `start_height` and `stop_height` arguments
  against the current tip, then resolves the start block hash with
  `findAncestorByHeight()`.

### 2. Reserve the rescan and synchronize with validation

- RPC entry points call `src/wallet/wallet.cpp`
  (`CWallet::BlockUntilSyncedToCurrentChain`) before starting. This drains the
  validation-interface queue up to at least the chain tip the user could have
  observed from earlier RPCs.
- `src/wallet/wallet.h` (`WalletRescanReserver`) is the single-wallet rescan
  gate. `reserve()` flips `fScanningWallet`, tracks elapsed time and progress,
  and prevents overlapping rescans on the same wallet.
- The same reserve gate is also used by `src/wallet/rpc/wallet.cpp`
  (`unloadwallet`), so unloads fail while a rescan is active instead of racing
  wallet destruction against the scan.
- Encrypted-wallet RPC rescans reserve with `with_passphrase=true`. Both
  `importdescriptors` and `rescanblockchain` take `m_relock_mutex` and call
  `EnsureWalletIsUnlocked()`, so keypool top-ups and any signing-provider work
  done during the rescan are not interrupted by relock timers.
- While `IsScanningWithPassphrase()` is true,
  `src/wallet/rpc/encrypt.cpp` (`walletlock`, `walletpassphrasechange`)
  refuses to change encryption state.

### 3. Attach notifications before startup scanning

- `src/wallet/wallet.cpp` (`CWallet::AttachChain`) registers
  `interfaces::Chain::Notifications` before scanning. The in-source comment is
  explicit about the reason: block connections must not be missed while the
  wallet is catching up.
- Startup rescans run with `cs_wallet` held for the whole `AttachChain()`
  sequence. New `blockConnected` notifications queue on the validation side
  until the lock is released, then are delivered afterward.
- The same comment also notes the consequence: after startup rescan the wallet
  is not fully caught up until those queued notifications run.
- `src/wallet/test/wallet_tests.cpp` (`CreateWallet`) covers both sides of
  this boundary: delayed notifications arriving after load and immediate
  notifications after the rescan, verifying there is no lost-update gap.

### 4. Scan blocks, optionally through block filters

- `src/wallet/wallet.cpp` (`CWallet::ScanForWalletTransactions`) asserts that
  a `WalletRescanReserver` is already active and initializes GUI/logging
  progress.
- If `interfaces::Chain::hasBlockFilterIndex(BlockFilterType::BASIC)` is true,
  the wallet builds a `FastWalletRescanFilter`. This filter is populated from
  every current descriptor scriptPubKey manager's scriptPubKeys.
- The fast filter is not static. `FastWalletRescanFilter::UpdateIfNeeded()`
  checks each ranged descriptor's `GetEndRange()` and adds newly derived
  scriptPubKeys when keypool top-ups happen during the rescan. The functional
  test `test/functional/wallet_fast_rescan.py` covers this top-up-sensitive
  behavior and checks that fast and slow rescans find the same transactions.
- For each candidate block, `ScanForWalletTransactions()` asks the chain
  interface for active-chain status and the next block hash before reading the
  block body. This is deliberate reorg protection: slow block reads can race
  chain changes.
- When a block filter says a block definitely has no matching script, the scan
  skips reading block data and simply advances `last_scanned_block`.
- When a block is fetched successfully, the wallet takes `cs_wallet` and calls
  `SyncTransaction(..., TxStateConfirmed{...}, fUpdate, rescanning_old_block=true)`
  for every transaction in block order.
- Because `rescanning_old_block=true`,
  `src/wallet/wallet.cpp` (`CWallet::ComputeTimeSmart`) assigns newly found
  historical transactions the block max time instead of current wall clock
  time. `test/functional/wallet_transactiontime_rescan.py` checks this
  restored-wallet timestamp behavior.
- If `save_progress=true`, the scan periodically writes an updated best-block
  locator to the wallet database. Startup rescans use this mode; manual
  `rescanblockchain` does not.

### 5. Failure, partial success, and mempool replay

- Read failures do not necessarily stop the scan immediately. If a block cannot
  be read, `ScanForWalletTransactions()` records `last_failed_block` and marks
  the result as `FAILURE`, but it can continue and still find later readable
  blocks. `src/wallet/test/wallet_tests.cpp`
  (`scan_for_wallet_transactions`) covers this with pruned block files.
- `src/wallet/wallet.cpp` (`CWallet::RescanFromTime`) converts a failed scan
  back into the earliest timestamp that may still contain undiscovered wallet
  transactions by looking up the failed block's max time and adding the same
  two-hour window.
- A full scan (`max_height` unset) always finishes by replaying current mempool
  transactions through `interfaces::Chain::requestMempoolTransactions(*this)`.
  A bounded rescan does not. This difference matters for unconfirmed wallet
  detection.
- `test/functional/wallet_rescan_unconfirmed.py` covers the mempool replay
  path, including a reorg case where a parent transaction re-enters the
  mempool before its child and the importing wallet still needs to discover
  both.
- `test/functional/wallet_anchor.py` covers bounded rescans in a different
  way: scanning only up to the block containing an anchor output surfaces the
  historical receive, but a later scan is still required to learn that the
  output was spent.

## Descriptor Import and Restore Specifics

- `src/wallet/rpc/backup.cpp` (`importdescriptors`) parses each descriptor,
  checks active/internal/range/label rules, stores it through
  `CWallet::AddWalletDescriptor()`, connects script-pubkey-manager notifiers,
  refreshes cached wallet TXOs, and then rescans from the earliest imported
  timestamp.
- `CWallet::AddWalletDescriptor()` updates an existing descriptor when IDs
  match, otherwise creates a new `DescriptorScriptPubKeyMan`, imports provided
  private keys, tops up the descriptor cache, and marks wallet balances dirty
  so already-known transactions can become `IsMine`.
- `src/wallet/scriptpubkeyman.cpp`
  (`DescriptorScriptPubKeyMan::GetTimeFirstKey`) returns a descriptor's stored
  creation time, so descriptor timestamps directly influence future
  birthday-based rescan skips.
- `src/wallet/rpc/backup.cpp` (`restorewallet`) does not implement its own scan
  logic. It restores the database file, then relies on the ordinary
  `LoadWallet()`/`AttachChain()` path. `src/wallet/wallet.cpp`
  (`CWallet::BackupWallet`) writes the best-block locator immediately before
  copying the file, which is why recent backups can still restore successfully
  near a prune boundary. `test/functional/wallet_backup.py` covers this case.

## Fund-Safety and Availability Implications

- `fund-safety`, `balance-miscompute`: rescans reconstruct wallet
  transactions, TXOs, and balances from chain data, but they do not recreate
  wallet metadata. `doc/managing-wallets.md` explicitly says labels and similar
  metadata cannot be recovered from a blockchain rescan.
- `fund-safety`, `stranded-funds`: `src/wallet/rpc/backup.cpp`
  (`importdescriptors`) warns that descriptor imports require a new wallet
  backup. `doc/managing-wallets.md` adds the operator-facing reason: after
  encryption or passphrase changes the keypool is flushed and a new HD seed is
  generated, so old backups may not recover later receives.
- `availability`: long-running descriptor imports can leave the wallet in a
  visibly partial state. The `importdescriptors` help text warns that other RPC
  calls may already show keys or addresses while related transactions are still
  missing. Progress is exposed through `src/wallet/rpc/wallet.cpp`
  (`getwalletinfo`, `scanning.duration`, `scanning.progress`).
- `availability`: wallet load, restore, manual rescans, and descriptor imports
  all fail when required block data is unavailable. The current code has
  explicit branches for pruning in `src/wallet/wallet.cpp`
  (`CWallet::AttachChain`) and `src/wallet/rpc/transactions.cpp`
  (`rescanblockchain`), and for assumeutxo background-sync gaps in the same
  locations plus `src/wallet/rpc/backup.cpp` (`importdescriptors`). The
  functional tests `test/functional/feature_pruning.py`,
  `test/functional/wallet_backup.py`, and
  `test/functional/wallet_assumeutxo.py` cover these failure paths.
- `availability`, `consistency`: `test/functional/wallet_reorgsrestore.py`
  covers startup rescans repairing wallet state after missed disconnection
  notifications during shutdown or crash recovery.

## Runtime and Build Gates

- Build-time: `src/interfaces/wallet.h` (`interfaces::MakeWalletLoader`) is
  only available when `ENABLE_WALLET` is enabled.
- Runtime disable switch: `src/wallet/init.cpp` (`-disablewallet`) prevents
  wallet loading and wallet RPC registration entirely.
- Fast-path acceleration: `src/wallet/rpc/backup.cpp` and
  `src/wallet/rpc/transactions.cpp` both document `-blockfilterindex=1` as the
  switch that makes rescans significantly faster.
- Cross-chain restore/load guard: `src/wallet/wallet.cpp`
  (`CWallet::AttachChain`) rejects wallets whose saved genesis block does not
  match the current chain unless `-walletcrosschain` is set.
- Encrypted-wallet rescans triggered through RPC require an unlocked wallet.
  `test/functional/wallet_transactiontime_rescan.py` covers the locked-wallet
  failure case for `rescanblockchain`.

## Locks and Thread Assumptions

- `src/wallet/wallet.h` documents `cs_wallet` as the main wallet lock, and the
  rescan code takes it whenever wallet transaction state, balances, or last
  processed block pointers are mutated.
- Startup rescans intentionally keep `cs_wallet` held in `AttachChain()`.
  Manual and import-triggered rescans do not hold a permanent wallet lock
  across the whole scan loop.
- `src/wallet/wallet.cpp` (`CWallet::ScanForWalletTransactions`) documents both
  cases:
  - with the permanent startup lock, blocks connected during the rescan are not
    rescanned inline and instead arrive later through queued
    `blockConnected` notifications
  - without that permanent lock, blocks added during the scan may be
    re-processed from live notifications if the wallet's last processed height
    advanced mid-scan
- `src/interfaces/chain.h` notes that synchronous mempool replay from
  `requestMempoolTransactions()` is not coordinated with asynchronous wallet
  notifications. The wallet side therefore has to tolerate duplicate add/remove
  style events during and after a rescan catch-up.

## Related Tests

- `src/wallet/test/wallet_tests.cpp`
  (`scan_for_wallet_transactions`, `CreateWallet`)
- `test/functional/wallet_fast_rescan.py`
- `test/functional/wallet_importdescriptors.py`
- `test/functional/wallet_rescan_unconfirmed.py`
- `test/functional/wallet_transactiontime_rescan.py`
- `test/functional/wallet_assumeutxo.py`
- `test/functional/wallet_backup.py`
- `test/functional/feature_pruning.py`
- `test/functional/wallet_reorgsrestore.py`
- `test/functional/wallet_anchor.py`

## Adjacent Pages

- `[[areas/wallet]]`
- `[[concepts/descriptors]]`
- `[[concepts/wallet-fund-safety]]`
- `[[concepts/assumeutxo]]`
- `[[areas/validation-and-chainstate]]`

## Sources Consulted

- `src/wallet/wallet.cpp` (`CWallet::LoadExisting`, `CWallet::AttachChain`,
  `CWallet::RescanFromTime`, `CWallet::ScanForWalletTransactions`,
  `FastWalletRescanFilter`, `CWallet::ComputeTimeSmart`,
  `CWallet::BlockUntilSyncedToCurrentChain`, `CWallet::BackupWallet`)
- `src/wallet/wallet.h` (`WalletRescanReserver`, `CWallet::ScanResult`,
  `CWallet::AbortRescan`)
- `src/wallet/rpc/backup.cpp`
  (`importdescriptors`, `restorewallet`, `importprunedfunds`)
- `src/wallet/rpc/transactions.cpp` (`rescanblockchain`, `abortrescan`)
- `src/wallet/rpc/wallet.cpp` (`getwalletinfo`, `unloadwallet`)
- `src/wallet/rpc/encrypt.cpp` (`walletlock`, `walletpassphrasechange`)
- `src/wallet/scriptpubkeyman.cpp`
  (`DescriptorScriptPubKeyMan::TopUp`, `GetTimeFirstKey`,
  `UpdateWalletDescriptor`)
- `src/interfaces/chain.h` (`requestMempoolTransactions`)
- `src/interfaces/wallet.h` (`interfaces::MakeWalletLoader`)
- `src/wallet/init.cpp`
- `doc/managing-wallets.md`
- `src/wallet/test/wallet_tests.cpp`
- `test/functional/wallet_fast_rescan.py`
- `test/functional/wallet_importdescriptors.py`
- `test/functional/wallet_rescan_unconfirmed.py`
- `test/functional/wallet_transactiontime_rescan.py`
- `test/functional/wallet_assumeutxo.py`
- `test/functional/wallet_backup.py`
- `test/functional/feature_pruning.py`
- `test/functional/wallet_reorgsrestore.py`
- `test/functional/wallet_anchor.py`
