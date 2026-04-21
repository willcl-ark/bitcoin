---
kind: concept
title: Fee Estimation
status: active
last_reviewed: 2026-04-21
paths:
  - src/policy/fees/block_policy_estimator.h
  - src/policy/fees/block_policy_estimator.cpp
  - src/rpc/fees.cpp
  - src/wallet/fees.cpp
tags:
  - fees
  - estimator
  - mempool
---

# Fee Estimation

## Summary

Bitcoin Core's fee estimator tracks how mempool transactions actually confirm
over time and turns that history into target-based feerate estimates for RPC
and wallet consumers. The core implementation is `CBlockPolicyEstimator` in
`src/policy/fees/block_policy_estimator.*`.

## Responsibilities and Invariants

- `CBlockPolicyEstimator` is a `CValidationInterface` consumer. It observes
  mempool additions, non-block removals, and block-confirmed removals to update
  its internal statistics.
- Estimates are persisted in `fee_estimates.dat`. The estimator can flush and
  reload its state, with age checks for stale files described in
  `block_policy_estimator.h`.
- `estimateSmartFee()` returns the estimator's main user-facing answer. The
  source comment documents it as combining multiple success thresholds and time
  horizons, with conservative mode requiring stronger long-horizon evidence.
- RPC users reach the estimator through `src/rpc/fees.cpp`
  (`estimatesmartfee`, `estimaterawfee`) and `src/rpc/server_util.cpp`
  (`EnsureFeeEstimator`, `EnsureAnyFeeEstimator`).
- Wallet fee selection reaches the same estimator through
  `interfaces::Chain::estimateSmartFee()` as implemented in
  `src/node/interfaces.cpp` and consumed in `src/wallet/fees.cpp`.
- The estimator is not always present. `src/init.cpp` only creates it when the
  node will process incoming transaction relay; the functional fee-estimation
  test asserts that `-blocksonly` disables `estimatesmartfee`.

## Important Code Paths

- Estimator core:
  `src/policy/fees/block_policy_estimator.h`,
  `src/policy/fees/block_policy_estimator.cpp`
- Node initialization:
  `src/init.cpp`
- RPC surface:
  `src/rpc/fees.cpp`, `src/rpc/server_util.cpp`
- Wallet consumers:
  `src/node/interfaces.cpp`, `src/wallet/fees.cpp`, `src/wallet/spend.cpp`

## Related Tests

- `src/test/policyestimator_tests.cpp`
- `test/functional/feature_fee_estimation.py`
- `test/functional/rpc_estimatefee.py`
- `src/test/fuzz/policy_estimator.cpp`
- `src/test/fuzz/policy_estimator_io.cpp`

## Adjacent Pages

- `[[areas/mempool-and-policy]]`
- `[[concepts/addrman]]`
- `[[areas/wallet]]`

## Sources Consulted

- `src/policy/fees/block_policy_estimator.h`
- `src/policy/fees/block_policy_estimator.cpp`
- `src/init.cpp`
- `src/rpc/fees.cpp`
- `src/rpc/server_util.cpp`
- `src/node/interfaces.cpp`
- `src/wallet/fees.cpp`
- `src/wallet/spend.cpp`
- `src/test/policyestimator_tests.cpp`
- `test/functional/feature_fee_estimation.py`
