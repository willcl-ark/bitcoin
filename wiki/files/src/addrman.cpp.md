---
kind: file
title: src/addrman.cpp
status: active
last_reviewed: 2026-04-21
paths:
  - src/addrman.cpp
tags:
  - networking
  - addrman
  - privacy
  - persistence
---

# src/addrman.cpp

## Role in the System

`src/addrman.cpp` implements Bitcoin Core's stochastic peer-address store. It
maintains the `new` and `tried` tables, tracks connection outcomes, chooses
outbound candidates, and serializes/deserializes the in-memory address manager
used by `peers.dat` through `src/addrdb.cpp`.

This file sits on an operator-privacy and resource boundary. Its bucketing
rules determine how hard it is for one network locality or ASN to dominate the
store, its timestamp/update rules affect what the node later gossips, and its
load path decides how much of `peers.dat` is preserved versus dropped when
bucketing inputs change.

## Important Types and Functions

- `AddrInfo::GetTriedBucket()`, `AddrInfo::GetNewBucket()`, and
  `AddrInfo::GetBucketPosition()` hash address data, source-group data, and
  `nKey` into tried/new bucket ids and slot positions. `GetTriedBucket()` uses
  `netgroupman.GetGroup(*this)` plus `ADDRMAN_TRIED_BUCKETS_PER_GROUP`;
  `GetNewBucket()` uses both the advertised address group and the source group
  plus `ADDRMAN_NEW_BUCKETS_PER_SOURCE_GROUP`.
- `AddrManImpl::AddrManImpl()` chooses the bucketing secret `nKey`. In
  deterministic mode it is fixed to `uint256{1}`; otherwise it is drawn from
  `insecure_rand.rand256()`. The public load path in `src/addrdb.cpp`
  (`LoadAddrman()`) enables deterministic mode only when
  `HasTestOption(args, "addrman")` is set.
- `AddrManImpl::AddSingle()` is the main new-table insertion path. It rejects
  non-routable addresses, removes `time_penalty` for self-announcements,
  updates `nTime` only when the new gossip is materially fresher, merges
  service bits, caps multiplicity at `ADDRMAN_NEW_BUCKETS_PER_ADDRESS`, and
  uses a `2^nRefCount` stochastic gate before increasing multiplicity for an
  existing new-table entry.
- `AddrManImpl::MakeTried()` moves an entry from `vvNew` into `vvTried`. If
  the tried slot is occupied, the incumbent is demoted back into one new-table
  position chosen from its current bucketing under `nKey` and
  `m_netgroupman`.
- `AddrManImpl::Good_()`, `ResolveCollisions_()`, and
  `SelectTriedCollision_()` implement the tried-table test-before-evict
  discipline. `Good_()` records success and either promotes the address or
  adds it to `m_tried_collisions`; `ResolveCollisions_()` later keeps or
  replaces the incumbent based on `ADDRMAN_REPLACEMENT`,
  `ADDRMAN_TEST_WINDOW`, recent success, and recent failed attempts.
- `AddrInfo::IsTerrible()` and `AddrInfo::GetChance()` are the main quality
  filters. `IsTerrible()` drops entries that are too old, far in the future,
  repeatedly failing, or never successful after too many attempts.
  `GetChance()` heavily penalizes very recent attempts and repeated failures.
- `AddrManImpl::Select_()` chooses between `new` and `tried` (50/50 when both
  are non-empty), picks a random bucket/starting position, filters by network
  if requested, and samples with probability `GetChance() * chance_factor`,
  increasing `chance_factor` by `1.2` after each miss.
- `AddrManImpl::GetAddr_()` samples from `vRandom`, optionally filtering out
  `IsTerrible()` entries. `AddrManImpl::Connected_()` updates `nTime` only if
  at least 20 minutes have elapsed; `SetServices_()` overwrites service flags
  for an existing entry.
- `AddrManImpl::Serialize()` and `Unserialize()` define the on-disk format:
  file format/compat bytes, `nKey`, `nNew`, `nTried`, encoded bucket count,
  compacted new/tried `AddrInfo` entries, per-new-bucket entry indexes, and
  the current asmap version. The serialization comment notes a maximum size of
  about 1.5 MiB.
- `AddrManImpl::CheckAddrman()` verifies the internal invariants tying
  `mapInfo`, `mapAddr`, `vRandom`, `vvNew`, `vvTried`, `nKey`, and
  `m_network_counts` together. Public methods call `Check()` before and after
  mutations when `-checkaddrman` enables consistency checks.

## Callers and Dependencies

- `src/addrdb.cpp` is the persistence boundary. `DumpPeerAddresses()` writes
  `AddrMan` to `peers.dat`, and `LoadAddrman()` constructs `AddrMan`,
  deserializes it, and recreates a fresh instance when the file is missing or
  version-incompatible.
- `src/net.cpp` is the main outbound-selection and retry caller. `CConnman`
  uses `SelectTriedCollision()` and `Select()` when choosing feeler and normal
  outbound destinations, `Attempt()` after real connection attempts, and
  `GetAddr()` when serving address samples.
- `src/net_processing.cpp` feeds live P2P observations back into addrman.
  Address-relay handling stores relayed addresses with a two-hour penalty,
  successful outbound handshake processing calls `Good()`, version/service
  updates call `SetServices()`, and `FinalizeNode()` calls `Connected()` only
  for full outbound peers.
- `src/rpc/net.cpp` exposes maintenance and inspection hooks. `addpeeraddress`
  inserts into new or tried, and `getrawaddrman` uses `GetEntries()` to expose
  raw table state.
- Bucketing depends on `NetGroupManager` in `src/netgroup.cpp`. Without asmap,
  `GetGroup()` uses legacy network-prefix groupings (for example IPv4 `/16`);
  with asmap, IPv4/IPv6 grouping is replaced by mapped ASN data where
  available. `GetAsmapVersion()` is persisted so `Unserialize()` can tell when
  stored new-table bucket assignments are no longer valid under the current
  asmap.
- Address encoding depends on `CAddress::V1_DISK`/`V2_DISK`, and the tables and
  constants used here are declared in `src/addrman.h` and
  `src/addrman_impl.h`.

## Related Tests

- Bucketing and asmap behavior:
  `src/test/addrman_tests.cpp`
  (`caddrinfo_get_tried_bucket_legacy`, `caddrinfo_get_new_bucket_legacy`,
  `caddrinfo_get_tried_bucket`, `caddrinfo_get_new_bucket`,
  `addrman_serialization`)
- Selection, multiplicity, and collision handling:
  `src/test/addrman_tests.cpp`
  (`addrman_select`, `addrman_select_by_network`, `addrman_select_special`,
  `addrman_new_collisions`, `addrman_new_multiplicity`,
  `addrman_tried_collisions`, `addrman_selecttriedcollision`,
  `addrman_noevict`, `addrman_evictionworks`)
- Filtering, timestamps, and metadata updates:
  `src/test/addrman_tests.cpp`
  (`addrman_terrible_many_failures`, `addrman_penalty_self_announcement`,
  `addrman_getaddr`, `getaddr_unfiltered`, `addrman_update_address`,
  `addrman_size`)
- Persistence and corruption handling:
  `src/test/addrman_tests.cpp`
  (`remove_invalid`, `load_addrman`, `load_addrman_corrupted`)
- Fuzz coverage:
  `src/test/fuzz/addrman.cpp`
  (`data_stream_addr_man`, `addrman`, `addrman_serdeser`)

## Notes or Risks

- Verified fact: bucket placement depends on the secret `nKey`, and
  `Serialize()` writes `nKey` to disk so `Unserialize()` can reconstruct the
  same layout later. Operational implication (inference): bucket unpredictably
  resists remote manipulation, but anyone who can read `peers.dat` also learns
  the current addrman bucketing key.
- Verified fact: asmap changes are a hard bucketing cutover. `Unserialize()`
  restores stored new-table buckets only when both the serialized bucket count
  and serialized asmap version match the current `NetGroupManager`; otherwise
  it logs that addrman is being re-bucketed and recomputes placements under the
  current grouping inputs. The unit test `addrman_serialization` confirms same
  asmap preserves positions while asmap/no-asmap transitions change them.
- Verified fact: `Good_()` updates success state but intentionally does not
  update `nTime`, and `Connected_()` only updates `nTime` on a 20-minute
  cadence. `src/net_processing.cpp` further avoids `Connected()` for feeler,
  inbound, and private-broadcast peers. Operational implication (inference):
  addrman is deliberately trying to learn which peers work without turning
  current connection state into precise gossip-visible timing data.
- Verified fact: `GetAddr_()` itself only applies optional network filtering
  and optional `IsTerrible()` filtering. Ban filtering and response caching live
  outside this file in `src/net.cpp`. Operational implication (inference):
  `src/addrman.cpp` provides randomized samples, but caller policy still
  determines how much address information is exposed to remote peers.
- Verified fact: load-time persistence is compact, not lossless. `Unserialize()`
  bounds-checks `nNew`/`nTried`, drops invalid tried entries that collide under
  current bucketing, prunes new entries whose `nRefCount` falls to zero, and
  re-runs `CheckAddrman()` before accepting the structure. The tests
  `remove_invalid`, `load_addrman`, and `load_addrman_corrupted` cover these
  cases.
- Verified fact: tried-table churn is intentionally bounded. Only
  `ADDRMAN_SET_TRIED_COLLISION_SIZE` pending collisions are kept, and
  `ResolveCollisions_()` prefers the incumbent if it succeeded within
  `ADDRMAN_REPLACEMENT`. This is part of addrman's eclipse-resistance
  discipline and reduces attacker-driven replacement of recently proven peers.
- Verified fact: resource usage is bounded structurally by
  `ADDRMAN_NEW_BUCKET_COUNT`, `ADDRMAN_TRIED_BUCKET_COUNT`, and
  `ADDRMAN_BUCKET_SIZE`, and the load path rejects impossible counts up front.
  The expensive part is `CheckAddrman()`, which walks the full structure and is
  therefore guarded by `m_consistency_check_ratio` / `-checkaddrman` and
  disabled by default.

## Sources Consulted

- `src/addrman.cpp`
- `src/addrman.h`
- `src/addrman_impl.h`
- `src/addrdb.cpp`
- `src/netgroup.cpp`
- `src/net.cpp`
- `src/net_processing.cpp`
- `src/rpc/net.cpp`
- `src/test/addrman_tests.cpp`
- `src/test/fuzz/addrman.cpp`
