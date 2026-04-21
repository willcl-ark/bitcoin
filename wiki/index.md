# Wiki Index

Repo snapshot:
- Branch: `llm-wiki`
- HEAD: `cd518aaee57`
- Status at bootstrap: new `wiki/` tree; unrelated untracked `.codex/`

## Overview

- `[[overview]]` - High-level map of subsystem boundaries, workflow pages, and build/test context.
- `[[log]]` - Append-only record of wiki ingests and maintenance passes.

## Areas

- `[[areas/consensus-and-script]]` - Consensus transaction and block checks, script execution, and the policy boundary around script flags.
- `[[areas/validation-and-chainstate]]` - Header/block acceptance, active-chain selection, UTXO updates, reorg handling, and assumeutxo-aware chainstate ownership.
- `[[areas/mempool-and-policy]]` - Local relay and mining policy, package/RBF/TRUC rules, and mempool storage/eviction behavior.
- `[[areas/p2p-and-networking]]` - Transport, peer management, headers/block/tx relay, address gossip, discouragement, and tx download scheduling.
- `[[areas/wallet]]` - Wallet loading, descriptor-only creation, wallet RPC integration, runtime gates, and offline tooling.
- `[[areas/rpc-rest-zmq-and-interfaces]]` - External request/notification surfaces and the internal interfaces that separate node, wallet, GUI, and IPC components.
- `[[areas/common-utils-and-configuration]]` - Args/config/settings handling plus reusable utility code shared across subsystems.
- `[[areas/testing]]` - Unit, wallet, GUI, kernel, fuzz, functional, and lint test layers.
- `[[areas/build-packaging-and-ci]]` - CMake, depends, CI scripts, deploy assets, and reproducible-build workflows.
- `[[areas/libbitcoinkernel-and-libraries]]` - The experimental `bitcoinkernel` boundary and bundled library roles.

## Concepts

- No concept pages yet.

## Workflows

- `[[workflows/transaction-acceptance]]` - Single-tx, package, RPC, and P2P paths into mempool admission.
- `[[workflows/block-validation-and-connection]]` - `ProcessNewBlock` through `ActivateBestChain`, including `ConnectBlock` and reorg repair.
- `[[workflows/initial-block-download]]` - IBD exit conditions, sync behavior, relay suppression, and assumeutxo interaction.

## Files

- No file pages yet.

## Sources

- No source summary pages yet.

## Investigations

- No investigation pages yet.
