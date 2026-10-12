# TCP holepunch specification issues

## Canonical draft compliance review, 2026-10-13

The canonical source is now b10c's draft 0.3.4 at
[`17bbcc637049b9ce63a0faac91e0a9330481193f`](https://github.com/0xB10C/bips/blob/17bbcc637049b9ce63a0faac91e0a9330481193f/bip-tcp-holepunch.md).
The remote branch tip was verified before this review. Core was reviewed at
`cb8d503095807784095ea5b4009f488e5c19b9a5`. The canonical draft includes all 15
earlier amendments below, plus the changes in drafts 0.3.2 through 0.3.4.
The implementation is not yet compliant with those new requirements.

### Required changes

1. **BLOCKING: replace the onion identity when the external address changes.**
   R-12.1 and I-12 prohibit announcing a new address under an identity already
   associated with the old one. `HolePunchExternal()` updates the endpoint at
   `src/net_processing.cpp:6156`, while announcements at line 6509 reuse the
   existing global service. `src/torcontrol.cpp:639` rotates only after 24 hours.
   Observers can therefore link successive ISP-assigned addresses. Retain the
   address binding for each announced identity across quorum loss, suppress
   changed-address announcements immediately, and obtain and publish a fresh
   service before resuming. Preserve every retiring service's cache overlap,
   including when multiple address changes occur within twenty minutes.
   Relevant implementation commits: `707845e2b59`, `6bbc0cb9b44`.

2. **BLOCKING: negotiate the lower feature version.** R-3.2 defines the field as
   the highest supported version and negotiates the minimum. At
   `src/net_processing.cpp:4349`, any nonzero remote version is ignored. A peer
   advertising maximum version 1 should negotiate version 0 with this node, but
   subsequent coordination messages instead cause disconnection. Parse the full
   payload and apply the rules for the negotiated version.
   Relevant implementation commit: `6bbc0cb9b44`.

3. **BLOCKING: choose a random default punch port.** The terminology and limits
   sections require a default chosen at startup from 49152 through 65535,
   excluding bad ports and fixed throughout the run. `src/init.cpp:2287` still
   uses the P2P port plus two; lines 1037 through 1045 also retain the obsolete
   overflow restriction. Choose the default once, keep explicit overrides, and
   update help and operator documentation.
   Relevant implementation commit: `85600f64714`.

4. **BLOCKING: do not self-announce a bad-port external endpoint.** R-6.5 now
   explicitly forbids this. Both announcement lists at
   `src/net_processing.cpp:6505` and line 6512 accept such endpoints, even though
   connectors reject them. A disposable regtest advertiser reproduced this by
   announcing port 27017 after two agreeing reports. Filter both profiles with
   `IsBadPort()` before selecting an announcement.
   Relevant implementation commit: `6bbc0cb9b44`.

### Recommendations and allowed choices

- **SHOULD-FIX:** R-3.5 recommends omitting the feature from feeler and
  address-fetch connections. The send condition at `src/net_processing.cpp:4066`
  does not exclude either type. Avoid disclosing participation on these
  short-lived connections. Relevant commit: `6bbc0cb9b44`.
- R-1.4's existing stable-source-address recommendation is supported through
  explicit `-punchbind`; the wildcard default does not automatically choose a
  stable address, particularly with IPv6 privacy addressing.
- The onion virtual port is now an implementation choice. Continuing to use the
  network's default P2P port is permitted and does not require a change.
- Existing rotation timing is relative to publication, rather than a common
  wall-clock boundary. The twenty-minute retirement and publication gate remain
  applicable when address-triggered replacement is added.
- Source and operator documentation still identify the old implementation pin.
  Update those references when the implementation adopts this canonical draft.

### Questions for the author

- R-12.1 does not clearly define whether one onion identity may advertise stable
  IPv4 and IPv6 endpoints together, or whether each family needs its own identity.
  This affects the design of address-triggered rotation. A loss of reporting
  quorum must not erase the remembered association and permit a later different
  address to reuse the old identity.
- R-11.2 counts requests that named a target, while the implementation counts
  requests that pass validation and lead to pairing. Clarify whether rejected
  requests count. The separate 17-second pairing cooldown already bounds
  successful sessions.

Wire encodings and the published vectors are unchanged. All 38 vector checks
pass. Disposable regtest probes also confirmed the version-negotiation failure,
the bad-port announcement, and the old default port. The probes and logs are
outside the repository; all their processes have stopped. This review changes
no protocol behavior and adds no functional tests.

## Earlier amendments incorporated into the canonical draft

The current implementation was written against the amended draft at
[`a9860ba2ab7cb048209bf69f1e313a31200e1aa2`](https://github.com/willcl-ark/bips/blob/a9860ba2ab7cb048209bf69f1e313a31200e1aa2/bip-tcp-holepunch.md).
The amendments are on `origin/holepunch-amendments` in
`/home/will/src/core/bitcoin/bips`, based on upstream draft commit
`d3e730bba038d04d9e95cb3633ac4fbe9b5713c2`. They change protocol behavior and
clarify state transitions without changing the wire encodings.

| Issue | Amendment | Commit |
| --- | --- | --- |
| Definition retained an external address with one reporter; R-2.2 and I-10 required two current independent reporters. | Require two connected reporters in distinct groups throughout. | `3e530ff` |
| State table could extend a session when the handshake progressed and omitted a handshake timeout. Profile B has no incoming punch to start its timer. | Absolute 17-second session deadline; stop attempts at 15 seconds; define Profile B's start on validated rendezvous acceptance. | `e66d0c9` |
| R-5.6 treated a tuple conflict as proof that accept succeeded, despite TIME_WAIT also causing conflicts. | Wait for an actual matching accepted socket, without retries or extending the deadline. | `b07aa40` |
| PUNCHPORT used connect/accept terminology despite simultaneous open allowing either delivery method for either protocol role. | Determine direction from the negotiated role, while requiring actual punch-port use without a proxy. | `7ba0902` |
| Profile A had no way to identify a coordination-only socket before normal handshake messages arrived. Early session success also closed temporary coordinator connections before the required lifetime. | Profile A uses ordinary P2P traffic; Profile B retains the strict allowlist. Keep successful temporary A connections for the full 17 seconds. | `831e7ec` |
| An existing coordinator could fail its own network-group diversity comparison. | Exclude only the exact coordinator connection; all other outbounds still count. | `becad7c` |
| Any punch cleared an outstanding rendezvous, allowing a second, redirecting punch to be treated as unsolicited. | Rejected responses retain the original target check and deadline. | `48f12e5` |
| Encoding rationale and coordinator table assumed at most one exchange per connection, contrary to the cooldown rules. | Permit repeated exchanges and remember the last punch time. | `9fb5127` |
| Receiving had no future timestamp bound, and a ten-minute onion retirement overlap omitted the accepted clock skew. | Apply freshness limits on receipt and relay, preserve timestamps, retain retiring services for twenty minutes, and stop announcing an identity at its rotation deadline. | `c9f5a93` |
| Profile B admission timer origin and shared limits during rotation were unspecified. | Count ten seconds from socket acceptance and cap all active/retiring services at eight connections in total. | `d9b5496` |
| Closing immediately after queuing punch could discard it. | Drain encoded bytes to the socket within the original deadline; start the responder immediately; cancel unfinished sessions on send failure. | `f8d24c0` |
| R-7.3 assumed every existing coordinator connection used the punch port, overlooking the ephemeral fallback. | Reuse only an outbound with PUNCHPORT sent and verify COORDINATOR support. | `a0bcdfb` |
| Overlapping eviction handoffs could each add an extra inbound connection. | Allow only one pending handoff per coordinator. | `016e068` |
| Valid wire vectors use documentation addresses and include a zero port, which policy must reject. | Distinguish decoding valid encodings from authorizing endpoint use. | `0314af5` |
| Retention could suppress ordinary safety and liveness checks for an entire announcement lifetime. | Protect against planned eviction and coordination cleanup; permit disconnects for protocol violations, failed liveness checks, and stalled block/header downloads. | `a9860ba` |

The twenty-minute onion overlap accommodates up to ten minutes of clock skew.
It does not promise reachability with arbitrary clock differences. The draft
continues to leave I2P-specific coordination mechanics outside its scope.

Validation: all 38 existing wire vectors pass. Rule references and state-table
columns were checked, and the amended rules received separate coordination and
Tor lifecycle reviews. No GitHub comments or pull requests were posted.

## Open issue found during runtime validation

In a live Profile B run, the advertiser established TCP before the connector
received `punch` over Tor. R-5.7 required the connector to close that unmatched
socket. R-5.8 then stopped the advertiser's attempts, and the connector's later
socket found no active session. Neither side completed the v2 handshake.

Proposed clarification: treat a socket that closes before receiving any BIP324
bytes as a failed attempt, retaining the session and reserved slot. Retry after
RETRY_MS within the original attempt window; during grace, accept matching
sockets only. Keep the absolute deadlines and tuple-conflict rule unchanged.
Draft 0.3.4 retains the original termination behavior. This recovery proposal is
not part of the canonical BIP or code and must not be treated as a compliance fix.
