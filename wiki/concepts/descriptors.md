---
kind: concept
title: Descriptors
status: active
last_reviewed: 2026-04-21
paths:
  - doc/descriptors.md
  - src/script/descriptor.h
  - src/script/descriptor.cpp
  - src/wallet/scriptpubkeyman.h
  - src/wallet/scriptpubkeyman.cpp
tags:
  - descriptors
  - wallet
  - script
---

# Descriptors

## Summary

Descriptors are the structured language Bitcoin Core uses to describe sets of
output scripts. They are both an RPC/user-facing language and an internal
wallet representation: the parser and expansion logic live in `src/script/`,
while descriptor wallets store and manage descriptors through descriptor-based
script pubkey managers.

## Responsibilities and Invariants

- `doc/descriptors.md` documents the language supported by the current tree,
  including `pk`, `pkh`, `wpkh`, `sh`, `wsh`, `tr`, multisig forms, Miniscript
  in supported contexts, xpub derivation, and checksums.
- `src/script/descriptor.h` defines the `Descriptor` interface. Important
  properties include whether a descriptor is ranged, whether it is solvable,
  and whether it can be rendered back to private or normalized form.
- `src/script/descriptor.cpp` implements:
  - `Parse(...)` to build descriptor objects from strings
  - `GetDescriptorChecksum(...)`
  - `InferDescriptor(...)` to derive a descriptor from a script and provider
  - expansion helpers such as `Expand(...)` and `ExpandFromCache(...)`
- Wallet integration is centered on descriptor script pubkey managers in
  `src/wallet/scriptpubkeyman.*`. Current wallet creation paths are
  descriptor-first, and descriptor state is part of the wallet database and key
  management logic.
- Descriptor RPC surfaces are spread across multiple RPC areas, including
  script/output helpers, blockchain scans, PSBT-related flows, and wallet
  backup/export/import paths.

## Important Code Paths

- Language and examples: `doc/descriptors.md`
- Parser and expansion core:
  `src/script/descriptor.h`, `src/script/descriptor.cpp`
- Wallet ownership and storage:
  `src/wallet/scriptpubkeyman.h`, `src/wallet/scriptpubkeyman.cpp`,
  `src/wallet/wallet.cpp`
- Descriptor-related RPC touchpoints:
  `src/rpc/output_script.cpp`, `src/rpc/blockchain.cpp`,
  `src/rpc/rawtransaction.cpp`, `src/wallet/rpc/backup.cpp`

## Related Tests

- `src/wallet/test/walletload_tests.cpp`
- `test/functional/wallet_multisig_descriptor_psbt.py`
- `test/functional/wallet_miniscript_decaying_multisig_descriptor_psbt.py`

## Adjacent Pages

- `[[areas/wallet]]`
- `[[areas/consensus-and-script]]`
- `[[concepts/script-verification]]`

## Sources Consulted

- `doc/descriptors.md`
- `src/script/descriptor.h`
- `src/script/descriptor.cpp`
- `src/wallet/scriptpubkeyman.h`
- `src/wallet/scriptpubkeyman.cpp`
- `src/wallet/wallet.cpp`
- `src/rpc/output_script.cpp`
- `src/rpc/blockchain.cpp`
- `src/rpc/rawtransaction.cpp`
- `src/wallet/rpc/backup.cpp`
