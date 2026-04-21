---
kind: concept
title: Script Verification
status: active
last_reviewed: 2026-04-21
paths:
  - src/script/interpreter.h
  - src/script/interpreter.cpp
  - src/script/sigcache.h
  - src/script/sigcache.cpp
  - src/policy/policy.h
  - src/validation.cpp
tags:
  - script
  - verification
  - sigcache
---

# Script Verification

## Summary

Script verification is the path from a spending transaction input to a boolean
answer about whether the spend satisfies the referenced output under a specific
flag set. The interpreter lives in `src/script/interpreter.*`, signature
verification caching lives in `src/script/sigcache.*`, and the main validation
callers live in `src/validation.cpp`.

## Responsibilities and Invariants

- `src/script/interpreter.h` defines the script verification flag space, from
  mandatory consensus checks through policy-only discouragement flags.
- `src/script/interpreter.cpp` (`VerifyScript`) executes `scriptSig`,
  `scriptPubKey`, P2SH handling, witness handling, cleanstack, and taproot
  logic over the correct `SigVersion`.
- `EvalScript()` is the opcode execution engine. `VerifyScript()` is the
  higher-level transaction-spend wrapper that composes script stages.
- `src/script/sigcache.h` / `sigcache.cpp`
  (`CachingTransactionSignatureChecker`) wrap signature checks with the shared
  signature cache.
- `src/policy/policy.h` defines the current split between
  `MANDATORY_SCRIPT_VERIFY_FLAGS` and `STANDARD_SCRIPT_VERIFY_FLAGS`. The latter
  extends the former with stricter relay/mempool policy checks such as
  `CLEANSTACK`, `MINIMALDATA`, `NULLFAIL`, and discouragement flags.
- `src/validation.cpp` (`CheckInputScripts`, `CScriptCheck::operator()`)
  packages the interpreter into transaction/block validation. The code comments
  explicitly note that only data committed by the spending transaction's witness
  hash is handed into `CScriptCheck`.

## Important Code Paths

- Flag definitions:
  `src/script/interpreter.h`, `src/policy/policy.h`
- Interpreter:
  `src/script/interpreter.cpp` (`EvalScript`, `VerifyScript`)
- Signature cache:
  `src/script/sigcache.cpp`
- Validation callers:
  `src/validation.cpp` (`CheckInputScripts`, `CScriptCheck::operator()`)
- Consensus flag consumers:
  `src/consensus/tx_verify.cpp`

## Related Tests

- `src/test/script_tests.cpp`
- `src/test/script_p2sh_tests.cpp`
- `src/test/script_segwit_tests.cpp`
- `src/test/sighash_tests.cpp`
- `src/test/txvalidationcache_tests.cpp`
- `src/test/fuzz/script_interpreter.cpp`

## Adjacent Pages

- `[[areas/consensus-and-script]]`
- `[[areas/mempool-and-policy]]`
- `[[concepts/descriptors]]`
- `[[workflows/transaction-acceptance]]`

## Sources Consulted

- `src/script/interpreter.h`
- `src/script/interpreter.cpp`
- `src/script/sigcache.h`
- `src/script/sigcache.cpp`
- `src/policy/policy.h`
- `src/validation.cpp`
- `src/consensus/tx_verify.cpp`
