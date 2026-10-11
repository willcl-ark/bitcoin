# TCP holepunch specification issues

The implementation uses the amended draft at
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
This proposal awaits a decision and is not part of the pinned BIP or code.
