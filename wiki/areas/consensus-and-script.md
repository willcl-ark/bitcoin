---
kind: area
title: Consensus and Script
status: active
last_reviewed: 2026-04-20
paths:
  - src/consensus/
  - src/script/
  - src/primitives/transaction.h
  - src/primitives/block.h
tags:
  - consensus
  - script
---

## Summary

This area defines context-free transaction and block validity, UTXO-contextual spend checks, and the script interpreter used for P2SH, segwit v0, taproot, and tapscript validation. The main current entry points are `src/consensus/tx_check.cpp` (`CheckTransaction`), `src/consensus/tx_verify.cpp` (`Consensus::CheckTxInputs`, `GetTransactionSigOpCost`), `src/validation.cpp` (`CheckBlock`, `CheckInputScripts`, `Chainstate::ConnectBlock`), and `src/script/interpreter.cpp` (`VerifyScript`, `EvalScript`).

## Responsibilities / Invariants

- `src/consensus/tx_check.cpp` (`CheckTransaction`) performs context-free transaction checks: non-empty inputs/outputs, size/weight bound using stripped serialization, output money ranges, duplicate input detection, and coinbase/non-coinbase input shape checks.
- `src/validation.cpp` (`CheckBlock`) performs context-free block checks before UTXO access: proof of work and signet solution, merkle-root and duplicate-tx malleation checks, coinbase placement, per-transaction `CheckTransaction` calls, and the legacy sigop bound.
- `src/consensus/tx_verify.cpp` (`Consensus::CheckTxInputs`) is the UTXO-contextual consensus layer: every prevout must exist, coinbase spends must be mature, input values must stay in range, and total input value must cover total output value.
- `src/script/interpreter.cpp` (`VerifyScript`) evaluates `scriptSig` and `scriptPubKey` sequentially on one stack, then applies P2SH, witness, cleanstack, and witness-malleation rules. `src/script/interpreter.h` (`PrecomputedTransactionData`, `GenericTransactionSignatureChecker`) provides the cached sighash material and signature-checking interface that script execution consumes.
- `src/validation.cpp` (`CheckInputScripts`) only feeds `CScriptCheck` with data committed by the spending transaction's witness hash, then uses the validation cache and, during block connection, can batch `CScriptCheck` work through `CCheckQueue<CScriptCheck>`.
- `src/consensus/tx_verify.cpp` (`GetTransactionSigOpCost`) combines legacy, P2SH, and witness sigop accounting. `src/script/interpreter.cpp` (`CountWitnessSigOps`) handles witness-specific sigop counting.
- `src/script/script.h` (`CScript::IsPushOnly`, `CScript::GetSigOpCount`) is consensus-relevant even though it is lower-level utility code, because P2SH validation and sigop accounting depend on it.
- The `src/script/` subtree also contains higher-level script tooling such as `src/script/sign.cpp`, `src/script/descriptor.cpp`, and `src/script/miniscript.cpp`; the consensus-critical execution engine in this area is centered on `src/script/interpreter.*` and its validation callers.

## Important Code Paths

- Mempool admission in `src/validation.cpp` runs `MemPoolAccept::PreChecks`, then `Consensus::CheckTxInputs`, `GetTransactionSigOpCost`, `MemPoolAccept::PolicyScriptChecks`, and `MemPoolAccept::ConsensusScriptChecks`. Both script-check stages use `CheckInputScripts`, but with different flag sets.
- Block validation in `src/validation.cpp` runs `CheckBlock`, `ContextualCheckBlockHeader`, `ContextualCheckBlock`, and `Chainstate::ConnectBlock`. `Chainstate::ConnectBlock` repeats the contextual spend, sequence-lock, sigop, and script checks that require the active UTXO set.
- Script execution flows through `CheckInputScripts` -> `CScriptCheck::operator()` -> `VerifyScript` -> `EvalScript`. Signature hashing and signature verification are supplied through `PrecomputedTransactionData`, `TransactionSignatureChecker`, and `CachingTransactionSignatureChecker`.

## Related Tests

- Unit: `src/test/script_tests.cpp`, `src/test/script_p2sh_tests.cpp`, `src/test/script_segwit_tests.cpp`, `src/test/sighash_tests.cpp`, `src/test/sigopcount_tests.cpp`, `src/test/txvalidationcache_tests.cpp`.
- Functional: `test/functional/feature_segwit.py`, `test/functional/p2p_segwit.py`, `test/functional/feature_taproot.py`, `test/functional/feature_cltv.py`, `test/functional/feature_dersig.py`, `test/functional/feature_nulldummy.py`, `test/functional/mempool_accept.py`, `test/functional/mempool_sigoplimit.py`, `test/functional/p2p_invalid_tx.py`.
- Fuzz: `src/test/fuzz/script_interpreter.cpp`, `src/test/fuzz/script_flags.cpp`, `src/test/fuzz/script_sigcache.cpp`.

## Adjacent Pages

- [[areas/validation-and-chainstate]]
- [[areas/mempool-and-policy]]
- [[workflows/transaction-acceptance]]
- [[workflows/block-validation-and-connection]]

## Sources Consulted

- `src/consensus/tx_check.cpp` (`CheckTransaction`)
- `src/consensus/tx_verify.cpp` (`Consensus::CheckTxInputs`, `GetTransactionSigOpCost`)
- `src/consensus/validation.h` (`GetWitnessCommitmentIndex`, validation state types)
- `src/script/interpreter.h` (`PrecomputedTransactionData`, `GenericTransactionSignatureChecker`)
- `src/script/interpreter.cpp` (`VerifyScript`, `CountWitnessSigOps`)
- `src/script/script.h` (`CScript::IsPushOnly`, `CScript::GetSigOpCount`)
- `src/script/sigcache.h` (`CachingTransactionSignatureChecker`)
- `src/validation.cpp` (`CheckInputScripts`, `MemPoolAccept::PolicyScriptChecks`, `MemPoolAccept::ConsensusScriptChecks`, `CheckBlock`, `Chainstate::ConnectBlock`)
