---
kind: concept
title: Addrman
status: active
last_reviewed: 2026-04-21
paths:
  - src/addrman.h
  - src/addrman.cpp
  - src/addrman_impl.h
tags:
  - addrman
  - p2p
  - peers
---

# Addrman

## Summary

`AddrMan` is Bitcoin Core's persisted address manager for outbound peer
selection. It stores candidate peers in privacy-preserving new/tried tables,
tracks attempts and successful connections, resolves tried-table collisions,
and returns randomized subsets for outbound dialing or address gossip.

## Responsibilities and Invariants

- `src/addrman.h` exposes the high-level API:
  `Add`, `Good`, `Attempt`, `ResolveCollisions`, `Select`, `GetAddr`, and
  `Connected`.
- `src/addrman_impl.h` owns the table layout and collision tracking. The
  implementation keeps separate new and tried structures, plus a bounded tried
  collision set.
- `Add(...)` inserts or updates candidate addresses from a given source and can
  apply time penalties to avoid over-trusting self-announcements or stale data.
- `Good(...)` promotes an address based on successful connection evidence.
- `Attempt(...)` records outbound dialing attempts and failure-related aging.
- `ResolveCollisions()` handles promotions that would collide in the tried
  table instead of overwriting existing entries blindly.
- `Select(...)` chooses a candidate for outbound use, optionally restricted to
  new-only or selected network sets.
- `GetAddr(...)` returns a randomized export subset instead of dumping the full
  table.

## Important Code Paths

- Public API:
  `src/addrman.cpp` (`AddrMan::Add`, `Good`, `Attempt`, `ResolveCollisions`,
  `Select`, `GetAddr`, `Connected`)
- Internal layout and bookkeeping:
  `src/addrman_impl.h`
- P2P consumers:
  `src/net.cpp`, `src/net_processing.cpp`

## Related Tests

- `src/test/addrman_tests.cpp`
- `test/functional/feature_addrman.py`
- `src/test/fuzz/addrman.cpp`

## Adjacent Pages

- `[[areas/p2p-and-networking]]`
- `[[concepts/operator-privacy]]`
- `[[concepts/fee-estimation]]`
- `[[files/src/addrman.cpp]]`

## Sources Consulted

- `src/addrman.h`
- `src/addrman.cpp`
- `src/addrman_impl.h`
- `src/net.cpp`
- `src/net_processing.cpp`
- `src/test/addrman_tests.cpp`
- `test/functional/feature_addrman.py`
