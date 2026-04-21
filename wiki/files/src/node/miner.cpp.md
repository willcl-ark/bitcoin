---
kind: file
title: src/node/miner.cpp
status: active
last_reviewed: 2026-04-21
paths:
  - src/node/miner.cpp
tags:
  - mining
  - block-assembly
  - templates
---

# src/node/miner.cpp

## Role in the System

`src/node/miner.cpp` assembles candidate blocks and supports the long-lived
mining template interface. Its main entry point,
`node::BlockAssembler::CreateNewBlock()`, builds a `node::CBlockTemplate` from
the active `Chainstate`, optional mempool contents, and caller-supplied
coinbase options, but does not solve proof of work.

The file is consensus-adjacent rather than consensus-owning. It relies on
helpers such as `TestBlockValidity()`, `GetNextWorkRequired()`,
`ChainstateManager::GenerateCoinbaseCommitment()`, and `BlockMerkleRoot()` to
avoid handing obviously bad templates to RPC or IPC mining clients.

## Important Types and Functions

- `node::CBlockTemplate` carries the candidate `CBlock`, per-transaction fees
  and sigops excluding the coinbase, `m_package_feerates`, and the exported
  `m_coinbase_tx` fields that mining clients use to build or verify the final
  coinbase.
- `node::BlockAssembler` owns one-shot template construction. `resetBlock()`
  seeds reserved block weight and sigops accounting, `AddToBlock()` updates
  fees/weight/sigops counters, and `addChunks()` walks mempool package
  candidates using `CTxMemPool::GetBlockBuilderChunk()`,
  `IncludeBuilderChunk()`, and `SkipBuilderChunk()`.
- `BlockAssembler::CreateNewBlock()` is the core assembly path. It locks
  `::cs_main` to read the active tip and version-bits state, optionally locks
  `m_mempool->cs` to select transactions, creates a placeholder coinbase at
  `block.vtx[0]`, replaces it with the final coinbase, calls
  `ChainstateManager::GenerateCoinbaseCommitment()`, fills header fields, and
  by default (`Options::test_block_validity = true`) calls
  `TestBlockValidity()`.
- The coinbase path records several externally visible invariants in
  `m_coinbase_tx`: the BIP34 height prefix in `script_sig_prefix`, optional
  dummy extranonce padding for low regtest heights, the witness reserved value
  when present, the remaining block reward, and required outputs such as the
  witness commitment.
- `GetMinimumTime()` and `UpdateTime()` own candidate-header time handling.
  `GetMinimumTime()` always applies the BIP94 timewarp minimum-time rule at
  difficulty-adjustment boundaries, and `UpdateTime()` also refreshes `nBits`
  on networks where `fPowAllowMinDifficultyBlocks` is true.
- `RegenerateCommitments()` repairs a block after its transaction list changes:
  it removes the old witness commitment output from the coinbase, reruns
  `GenerateCoinbaseCommitment()`, and recomputes `hashMerkleRoot`.
- `AddMerkleRootAndCoinbase()` is the submit path used by the mining
  interface. It swaps in the caller-supplied coinbase, updates
  version/time/nonce, recomputes the merkle root, and clears cached validation
  flags on the `CBlock`.
- `WaitAndCreateNewBlock()`, `InterruptWait()`, `GetTip()`,
  `WaitTipChanged()`, and `CooldownIfHeadersAhead()` implement the long-lived
  mining API on top of `KernelNotifications::m_tip_block_cv`, including
  tip-change waits, fee-threshold refreshes, and the post-IBD/header-ahead
  cooldown behavior.

## Callers and Dependencies

- `node::MinerImpl::createNewBlock()` in `src/node/interfaces.cpp` is the main
  caller. It optionally waits out IBD/header-ahead cooldown, applies startup
  args with `ApplyArgsManOptions()`, and constructs `BlockAssembler`.
- `node::BlockTemplateImpl::waitNext()` and `interruptWait()` in
  `src/node/interfaces.cpp` wrap `WaitAndCreateNewBlock()` and
  `InterruptWait()`. `submitSolution()` wraps `AddMerkleRootAndCoinbase()` and
  sends the reconstructed block to `ChainstateManager::ProcessNewBlock()`.
- `src/rpc/mining.cpp` uses this file in multiple user-facing RPC paths:
  `generateBlocks()` and `generateblock()` create templates through the mining
  interface, `generateblock()` calls `RegenerateCommitments()` after appending
  explicit transactions, and `getblocktemplate` uses `UpdateTime()` and
  `GetMinimumTime()` when serving template metadata.
- The file depends on chainstate and block-index state under `::cs_main`,
  mempool package-selection state under `CTxMemPool::cs`, consensus helpers
  including `IsFinalTx()` and `GetNextWorkRequired()`, validation through
  `TestBlockValidity()`, and `KernelNotifications` tip tracking for wakeups and
  interrupts.

## Related Tests

- `src/test/miner_tests.cpp`
  `BOOST_AUTO_TEST_CASE(CreateNewBlock_validity)` checks that freshly created
  templates need a recomputed merkle root before validation, alternate
  correctly between direct `ProcessNewBlock()` submission and the mining
  interface `submitSolution()` path, and reject unsolved proof of work.
- `src/test/miner_tests.cpp`
  `MinerTestingSetup::TestBasicMining()` covers failure modes that should be
  caught during block assembly or self-validation, including excessive sigops,
  missing inputs, duplicate coinbase entries, double spends, invalid P2SH
  spends, and non-final transaction handling.
- `src/test/miner_tests.cpp`
  `TestPackageSelection()` and `TestPrioritisedMining()` exercise `addChunks()`
  package ordering, ancestor feerate handling, minimum-fee filtering, and
  prioritisation effects.
- `src/test/testnet4_miner_tests.cpp`
  `MiningInterface` checks that template timestamps follow mocked `NodeClock`
  time and that `waitNext()` returns a fresh template once the 20-minute
  minimum-difficulty rule becomes relevant on testnet4.
- `src/test/validation_block_tests.cpp`
  `witness_commitment_index` exercises `GetWitnessCommitmentIndex()`, which
  `RegenerateCommitments()` relies on when stripping and rebuilding the coinbase
  witness commitment.
- `test/functional/interface_ipc_mining.py`
  covers early-startup `createNewBlock()`, cooldown waiting and interruption,
  `waitNext()` fee-threshold refreshes, per-template reserved-weight overrides,
  and `submitSolution()` rejection when the submitted coinbase omits witness
  data.
- `test/functional/interface_ipc.py`
  exercises cancellation and concurrent `BlockTemplate.waitNext()` calls,
  guarding the wait path against disconnect- and queueing-related regressions.
- `test/functional/mining_basic.py`
  checks user-visible effects of `ApplyArgsManOptions()` and
  `BlockAssembler::m_last_block_*`, including `-blockversion`,
  `-blockmintxfee`, `currentblocktx`, and `currentblockweight`.

## Notes or Risks

- `crash / hard-fail`: `CreateNewBlock()` mixes a debug `assert()` on the
  active tip with always-on `Assert()` checks for positive height,
  witness-stack shape, and witness-commitment output bounds. If those internal
  invariants are violated, template creation stops with an abort instead of a
  recoverable error.
- `invalid template / wasted work`: with the default
  `Options::test_block_validity = true`, block assembly re-runs
  `TestBlockValidity()` before returning a template. `src/validation.cpp`
  explicitly notes there is a similar check here to stop locally generated
  invalid blocks even if a policy-layer bug let a bad transaction reach the
  mempool.
- `offline / stalled miners`: `WaitAndCreateNewBlock()` and `WaitTipChanged()`
  intentionally release `KernelNotifications::m_tip_block_mutex` before taking
  `::cs_main` or calling `GetTip()`, to avoid deadlocks. Regressions here can
  wedge `Mining::createNewBlock()` or `BlockTemplate::waitNext()` for RPC or
  IPC miners.
- `resource misuse`: `addChunks()` limits repeated failed package attempts when
  a block is nearly full, and `WaitAndCreateNewBlock()` only performs its
  expensive fee-comparison rebuild once per second. Those heuristics matter
  when the mempool is large or many clients are polling for better templates.
- `availability under adversarial headers`: `CooldownIfHeadersAhead()` caps the
  cooldown window between 3 and 20 seconds while headers extend the current
  tip, specifically to avoid indefinite mining stalls if a peer announces a
  header but delays the corresponding block.
- `template correctness / payout safety`: `CreateNewBlock()`,
  `RegenerateCommitments()`, and `AddMerkleRootAndCoinbase()` must keep the
  coinbase, witness commitment, merkle root, and header fields synchronized. If
  callers mutate `block.vtx` without regenerating commitments, or submit a
  coinbase missing required witness data, the resulting block is rejected. The
  caller-provided `coinbase_output_script` also becomes the subsidy
  destination, so mistakes here can waste mining time or send rewards to the
  wrong script.
- `privacy-sensitive mining behavior`: this file does not choose or hide a
  payout destination. It copies the caller-provided `coinbase_output_script`
  into the template coinbase and exports coinbase-construction fields through
  `m_coinbase_tx`, so distinctive payout scripts or coinbase customizations are
  intentionally exposed to whichever mining client consumes the template.

## Sources Consulted

- `src/node/miner.cpp`
- `src/node/miner.h`
- `src/node/interfaces.cpp`
- `src/node/types.h`
- `src/node/kernel_notifications.h`
- `src/rpc/mining.cpp`
- `src/validation.cpp`
- `src/test/miner_tests.cpp`
- `src/test/testnet4_miner_tests.cpp`
- `src/test/validation_block_tests.cpp`
- `test/functional/interface_ipc.py`
- `test/functional/interface_ipc_mining.py`
- `test/functional/mining_basic.py`
