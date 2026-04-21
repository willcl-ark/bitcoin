---
kind: investigation
title: Security Review Concerns
status: active
last_reviewed: 2026-04-21
tags:
  - security
  - review
  - concerns
---

# Security Review Concerns

## Summary

This page aggregates the security-relevant concerns surfaced by the current
wiki pass. It is a review rollup, not a bug tracker: the items below mix
confirmed facts about current behavior with inferences about where risk or weak
assurance remains.

Current status: I did not identify a confirmed exploitable defect in the
consulted sources. The highest-signal concerns are availability boundaries,
operator-privacy surfaces, and fund-safety-adjacent code paths whose direct
coverage is thinner than their impact.

## Current Status

- Confirmed defects from the consulted sources: none.
- High-signal review concerns:
  - low-disk and write-failure behavior around
    `src/validation.cpp` (`Chainstate::FlushStateToDisk`)
  - reorg spillover and disconnect-pool handling around
    `src/kernel/disconnected_transactions.cpp` and
    `src/validation.cpp` (`MaybeUpdateMempoolForReorg`)
  - HTTP ingress, wallet URI routing, and request logging around
    `src/httpserver.cpp`, `src/httprpc.cpp`, and
    `src/wallet/rpc/util.cpp`
  - wallet post-construction state mutation in `src/wallet/wallet.cpp`
  - socket-loop backpressure and receive-queue shedding in `src/net.cpp`

## Concern Map

| Concern | Type | Impact | Current assessment | Primary pages |
| --- | --- | --- | --- | --- |
| `src/validation.cpp` (`Chainstate::FlushStateToDisk`) and nearby prune/write-failure paths | `coverage gap`, `availability review concern` | `crash`, `offline`, `resource` | No defect confirmed, but this is a fatal low-disk boundary with thinner direct failure coverage than its impact suggests. | `[[investigations/critical-test-coverage-gaps]]`, `[[concepts/resource-exhaustion-and-backpressure]]`, `[[workflows/node-startup-and-shutdown]]` |
| `src/kernel/disconnected_transactions.cpp` plus `src/validation.cpp` reorg spillover paths | `coverage gap`, `resource review concern` | `offline`, `resource` | No defect confirmed, but disconnect-pool bounds and reorg reinsertion rely heavily on top-level functional coverage. | `[[investigations/critical-test-coverage-gaps]]`, `[[concepts/resource-exhaustion-and-backpressure]]` |
| `src/httpserver.cpp`, `src/httprpc.cpp`, and `src/wallet/rpc/util.cpp` ingress and routing boundary | `operational privacy surface`, `coverage gap` | `operator-privacy`, `offline`, `resource` | Wallet names travel in `/wallet/<walletname>`, failed auth logs peer addresses, and the combined allowlist/dispatch/logging boundary is only indirectly covered. | `[[concepts/rpc-authentication-and-wallet-routing]]`, `[[concepts/operator-privacy]]`, `[[investigations/critical-test-coverage-gaps]]` |
| `src/wallet/wallet.cpp` (`CommitTransaction`, `MarkReplaced`, `SetSpentKeyState`) | `fund-safety review concern`, `coverage gap` | `fund-loss`, `wrong-transaction`, `sender/receiver-privacy` | No defect confirmed, but direct tests are weaker here than around transaction construction and PSBT update. | `[[concepts/wallet-fund-safety]]`, `[[investigations/critical-test-coverage-gaps]]`, `[[files/src/wallet/wallet.cpp]]` |
| `src/net.cpp` socket-loop backpressure (`SocketHandlerConnected`, `MarkReceivedMsgsForProcessing`, `PollMessage`) | `availability review concern`, `coverage gap` | `offline`, `resource` | No defect confirmed, but high-water receive/suppress-read branches look less directly exercised than adjacent parser and handshake code. | `[[concepts/resource-exhaustion-and-backpressure]]`, `[[investigations/critical-test-coverage-gaps]]`, `[[files/src/net.cpp]]` |
| Local observability knobs such as `-logips`, `-capturemessages`, and net tracepoints | `operational privacy surface` | `operator-privacy`, `topology correlation`, `operator fingerprinting` | This is current behavior, not a newly found bug. Operators can enable modes that write durable local metadata. | `[[concepts/operator-privacy]]` |
| `src/node/txdownloadman_impl.cpp` orphan-resolution TODOs | `review hotspot` | `resource`, `offline` | Not a confirmed bug. The current code explicitly notes that orphan-resolution-specific delays and limits still need dedicated treatment. | `[[concepts/resource-exhaustion-and-backpressure]]`, `[[files/src/node/txdownloadman_impl.cpp]]`, `[[files/src/node/txorphanage.cpp]]` |

## What This Page Is Not Saying

- It does not claim that any item above is a proven remote exploit.
- It does not replace checking the cited code paths before making risky
  conclusions.
- It does not collapse test gaps, privacy tradeoffs, and confirmed correctness
  bugs into one severity bucket.

## Adjacent Pages

- `[[investigations/actionable-security-work-items]]`
- `[[investigations/critical-codepaths-priority-map]]`
- `[[investigations/critical-test-coverage-gaps]]`
- `[[concepts/operator-privacy]]`
- `[[concepts/rpc-authentication-and-wallet-routing]]`
- `[[concepts/resource-exhaustion-and-backpressure]]`
- `[[concepts/wallet-fund-safety]]`

## Sources Consulted

- `wiki/investigations/critical-codepaths-priority-map.md`
- `wiki/investigations/critical-test-coverage-gaps.md`
- `wiki/concepts/operator-privacy.md`
- `wiki/concepts/rpc-authentication-and-wallet-routing.md`
- `wiki/concepts/resource-exhaustion-and-backpressure.md`
- `wiki/concepts/wallet-fund-safety.md`
- `wiki/files/src/net.cpp.md`
- `wiki/files/src/node/txdownloadman_impl.cpp.md`
- `wiki/files/src/node/txorphanage.cpp.md`
- `wiki/files/src/wallet/wallet.cpp.md`
- `src/validation.cpp`
- `src/httpserver.cpp`
- `src/httprpc.cpp`
- `src/wallet/rpc/util.cpp`
- `src/wallet/wallet.cpp`
- `src/net.cpp`
- `src/kernel/disconnected_transactions.cpp`
