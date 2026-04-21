---
kind: investigation
title: Actionable Security Work Items
status: active
last_reviewed: 2026-04-21
tags:
  - security
  - action-items
  - backlog
---

# Actionable Security Work Items

## Summary

This page turns the current security-review concerns into a concrete work
backlog. The emphasis is on items that either raise confidence quickly by
adding focused tests, or reduce privacy/availability risk by hardening exposed
boundaries.

The top of the backlog is test-heavy on purpose. For the current tree, the
highest-signal concerns are mostly about weak assurance on important
boundaries, not about a single confirmed exploit.

## Highest-Priority Work

| Priority | Work item | Type | Target code | Exit criteria |
| --- | --- | --- | --- | --- |
| `P1` | Add failure-injection or harness coverage for low-disk and write-failure behavior in chainstate flush paths. | `test` | `src/validation.cpp` (`Chainstate::FlushStateToDisk`), `src/node/blockstorage.cpp` | A focused unit or functional test forces low-disk or write failure and asserts the current fail-closed shutdown or abort behavior. |
| `P1` | Deepen reorg spillover coverage for disconnect-pool bounds and mempool re-add behavior. | `test` | `src/kernel/disconnected_transactions.cpp`, `src/validation.cpp` (`DisconnectTip`, `MaybeUpdateMempoolForReorg`) | Coverage no longer relies mostly on one top-level functional scenario; at least one lower-level targeted test or fuzz target exists. |
| `P1` | Add targeted HTTP ingress boundary tests that cover allowlist, queue rejection, warmup, and wallet URI routing together. | `test` | `src/httpserver.cpp` (`http_request_cb`), `src/httprpc.cpp` (`HTTPReq_JSONRPC`), `src/rpc/server.cpp`, `src/wallet/rpc/util.cpp` | Tests explicitly cover `POST`-only rejection, warmup rejection, queue-depth rejection, and wallet URI behavior at the HTTP/RPC boundary. |
| `P1` | Add focused wallet-state mutation tests around commit, replacement metadata, and spent-state bookkeeping. | `test` | `src/wallet/wallet.cpp` (`CommitTransaction`, `MarkReplaced`, `SetSpentKeyState`) | There are direct tests for post-construction wallet mutation invariants rather than only indirect RPC coverage. |
| `P1` | Add explicit socket-loop backpressure tests for receive high-water and send-before-read suppression. | `test` | `src/net.cpp` (`SocketHandlerConnected`, `CNode::MarkReceivedMsgsForProcessing`, `CNode::PollMessage`) | Tests drive `fPauseRecv`, receive-queue high-water conditions, and the branch that stops reading while sends still have data left. |

## Hardening Candidates

| Priority | Work item | Type | Target code | Exit criteria |
| --- | --- | --- | --- | --- |
| `P2` | Reduce or eliminate wallet-name leakage through `/wallet/<walletname>` request paths, or make logging explicitly suppress that metadata. | `hardening` | `src/httpserver.cpp`, `src/httprpc.cpp`, `src/wallet/rpc/util.cpp` | Wallet names are no longer trivially preserved in HTTP logs, or the remaining behavior is explicitly documented and tested. |
| `P2` | Treat privacy-sensitive observability modes as a first-class operator-safety topic. | `hardening`, `docs` | `src/init/common.cpp`, `src/net.cpp`, `src/net_processing.cpp` | Operators can easily discover that `-logips`, `-capturemessages`, and tracepoints create durable metadata. |
| `P2` | Review orphan-resolution-specific rate limiting and delays instead of relying only on the normal tx announcement logic. | `review`, `hardening` | `src/node/txdownloadman_impl.cpp` (`MaybeAddOrphanResolutionCandidate`) | The current TODO is either resolved in code or replaced with an explicit, documented design decision. |

## Review Support Work

| Priority | Work item | Type | Target code | Exit criteria |
| --- | --- | --- | --- | --- |
| `P3` | Add a file page for `src/httpserver.cpp`. | `wiki` | `src/httpserver.cpp` | The wiki has a durable page for the actual HTTP allowlist, logging, queueing, and shutdown boundary. |
| `P3` | Add a file page for `src/wallet/rpc/util.cpp`. | `wiki` | `src/wallet/rpc/util.cpp` | The wiki has a durable page for wallet URI extraction, default-wallet selection, and unlock checks. |
| `P3` | Revisit the critical-concerns rollup after each new test or hardening change. | `process` | `wiki/investigations/security-review-concerns.md` | The rollup stays current and no longer carries stale concerns that were already addressed. |

## Suggested Order

1. Finish the `P1` test items before trying to redesign behavior.
2. Re-evaluate whether any `P1` item exposes a real bug once the new coverage
   exists.
3. Tackle the `P2` hardening items with the new tests in place.
4. Keep the `P3` review-support items lightweight so they do not crowd out
   actual test and hardening work.

## Adjacent Pages

- `[[investigations/security-review-concerns]]`
- `[[investigations/critical-codepaths-priority-map]]`
- `[[investigations/critical-test-coverage-gaps]]`
- `[[concepts/operator-privacy]]`
- `[[concepts/rpc-authentication-and-wallet-routing]]`
- `[[concepts/resource-exhaustion-and-backpressure]]`
- `[[concepts/wallet-fund-safety]]`

## Sources Consulted

- `wiki/investigations/security-review-concerns.md`
- `wiki/investigations/critical-codepaths-priority-map.md`
- `wiki/investigations/critical-test-coverage-gaps.md`
- `wiki/concepts/operator-privacy.md`
- `wiki/concepts/rpc-authentication-and-wallet-routing.md`
- `wiki/concepts/resource-exhaustion-and-backpressure.md`
- `wiki/concepts/wallet-fund-safety.md`
