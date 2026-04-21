# Wiki Index

## Overview

- `[[overview]]` - High-level map of subsystem boundaries, workflow pages, and build/test context.
- `[[log]]` - Append-only record of wiki ingests and maintenance passes.

## Areas

- `[[areas/consensus-and-script]]` - Consensus transaction and block checks, script execution, and the policy boundary around script flags.
- `[[areas/validation-and-chainstate]]` - Header/block acceptance, active-chain selection, UTXO updates, reorg handling, and assumeutxo-aware chainstate ownership.
- `[[areas/mempool-and-policy]]` - Local relay and mining policy, package/RBF/TRUC rules, and mempool storage/eviction behavior.
- `[[areas/p2p-and-networking]]` - Transport, peer management, headers/block/tx relay, address gossip, discouragement, and tx download scheduling.
- `[[areas/mining-and-block-assembly]]` - Candidate block construction, mining RPCs, and internal template interfaces.
- `[[areas/wallet]]` - Wallet loading, descriptor-only creation, wallet RPC integration, runtime gates, and offline tooling.
- `[[areas/rpc-rest-zmq-and-interfaces]]` - External request/notification surfaces and the internal interfaces that separate node, wallet, GUI, and IPC components.
- `[[areas/common-utils-and-configuration]]` - Args/config/settings handling plus reusable utility code shared across subsystems.
- `[[areas/gui]]` - Qt application startup, node/wallet models, wallet views, and GUI-specific tests.
- `[[areas/testing]]` - Unit, wallet, GUI, kernel, fuzz, functional, and lint test layers.
- `[[areas/build-packaging-and-ci]]` - CMake, depends, CI scripts, deploy assets, and reproducible-build workflows.
- `[[areas/libbitcoinkernel-and-libraries]]` - The experimental `bitcoinkernel` boundary and bundled library roles.

## Concepts

- `[[concepts/chainstate]]` - Chainstate ownership, UTXO-set boundaries, and the meanings of current/historical/validated views.
- `[[concepts/assumeutxo]]` - Snapshot loading, dual-chainstate validation phases, and cache rebalancing during snapshot sync.
- `[[concepts/descriptors]]` - Descriptor language, parser/expansion logic, and wallet integration.
- `[[concepts/script-verification]]` - Interpreter flow, script flags, sigcache use, and consensus-vs-policy verification boundaries.
- `[[concepts/addrman]]` - Bucketized peer-address storage, promotion, selection, and export behavior.
- `[[concepts/fee-estimation]]` - Policy estimator ownership, persistence, and wallet/RPC consumers.

## Workflows

- `[[workflows/transaction-acceptance]]` - Single-tx, package, RPC, and P2P paths into mempool admission.
- `[[workflows/block-validation-and-connection]]` - `ProcessNewBlock` through `ActivateBestChain`, including `ConnectBlock` and reorg repair.
- `[[workflows/initial-block-download]]` - IBD exit conditions, sync behavior, relay suppression, and assumeutxo interaction.
- `[[workflows/rpc-request-handling]]` - HTTP JSON-RPC startup, dispatch through `tableRPC`, wallet URI routing, and the REST boundary.

## Files

- `[[files/src/validation.cpp]]` - File-level map of the central validation integration unit.

## Sources

- No source summary pages yet.

## Investigations

- No investigation pages yet.
