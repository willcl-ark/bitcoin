# Wallet

Guide to working with Bitcoin Core's wallet subsystem. The wallet lives in `src/wallet` and is accessed through public interfaces; its internals never depend on RPC code.

## Architecture & layering

- Keep wallet implementation in `src/wallet/` and expose it through stable interfaces (`src/interfaces/`).
- **Wallet internals must not depend on RPC code.** Cyclic dependencies arise when wallet code reaches into the RPC layer. Wallet RPC methods belong in `src/wallet/rpc/`; they are either wallet or non-wallet, never conditional hybrids.
- Wallet RPCs that require current chainstate must call `BlockUntilSyncedToCurrentChain`.
- Use `CWallet::Create()` for wallet loading in tests and benchmarks. Avoid calling `CWallet::LoadWallet()` directly.
- Pass specific values (e.g., file path) rather than the entire `ArgsManager` to wallet functions — this reduces header dependency chains.
- Keep MuSig2-related functionality in `musig.cpp`, not `key.cpp`. Share secp256k1 signing contexts rather than introducing private ones in kernel modules.

## Format & compatibility

- Preserve wallet file format and cross-version compatibility. When modifying serialization or record formats, add cross-version compatibility tests covering upgrade/downgrade scenarios (create wallet in feature branch, downgrade, operate, upgrade again).
- Prefer ranged descriptors (e.g., `xpub.../0/*` with range) over multipath specifiers to avoid importing many descriptors unnecessarily.

## Coin selection

- Set iteration limits proportional to the UTXO pool size (e.g., at least 10×).
- Use separate named constants per algorithm (e.g., `TOTAL_TRIES_CG` for CoinGrinder) since algorithms differ in solving power per iteration.

## Testing

- Guard wallet-dependent test cases individually with `is_wallet_compiled()` rather than skipping the entire test file.
- Keep wallet tests separate from consensus/functional tests.
- In unit tests using hardcoded size values, match real wallet defaults rather than arbitrary numbers.
- For wallet fuzz targets, set keypool size to 1 to avoid performance issues.
- When introducing code analogous to existing logic (e.g., replacing `IsMine` in migration), add fuzz tests comparing old and new implementations.
- For error-recovery tests, verify that misleading messages from prior states are absent from the error response.
- When removing an assertion or making a non-obvious change, explain why it is safe in the commit description.

Refer to [`doc/descriptors.md`](../doc/descriptors.md) for descriptor specifications and [`src/wallet/README.md`](../src/wallet/README.md) for the wallet architecture overview.
