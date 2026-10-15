# Experimental IPv4 TCP hole punching

This implements the version-1 profile of the coordinatorless holepunch draft
(`bip-holepunch-v6.md`, version 0.3.0). The experimental BIP155 network ID `0x08`
has not been assigned for public use. Descriptor relay is therefore restricted
to regtest and custom signets; mainnet, testnets and public signet ignore these
records. Ordinary outbound selection and source ports are unchanged.

To participate on an isolated network, configure:

```ini
[regtest]
holepunch=1
holepunchbind=192.168.1.10:38334
```

Replace the address with an IPv4 address assigned to the local host, and choose
a nonzero TCP port separate from its Bitcoin listening bindings. Participation
requires direct IPv4 connectivity and v2 transport. A configured IPv4 proxy or
an `onlynet` restriction excluding IPv4 prevents startup with hole punching.
Nodes on the experimental networks relay valid descriptors even when they do
not participate in punching.

The node opens up to three auxiliary connections from this binding to public
IPv4 peers on the same chain, selected from AddrMan in distinct network groups.
Their `version` messages report the external IP address and port they observe.
At least two connected observers must agree before the node advertises an
endpoint or starts an attempt. Observers need no special protocol support and
do not coordinate attempts. The oldest observer is replaced every ten minutes,
with up to four connections allowed during replacement. Reports expire after
thirty minutes. These connections appear as `holepunch-observer` in
`getpeerinfo`, use their own allowance, and only exchange handshake and keepalive
messages locally.

Each process waits until the next UTC hour before originating its first
descriptor, then waits at least five seconds past the epoch boundary. This
avoids replacing a descriptor whose ephemeral secret was lost on restart.
Endpoint agreement alone does not make the node publicly reachable. Descriptors
are kept separately from AddrMan, expire every hour, and are never dialed as
ordinary outbound peers.

Two participants retaining each other's descriptors derive the same schedule
and BIP324 initiator/responder roles. Eligibility and group priority depend on
the public IPv4 pair; changing keys or ports cannot improve them. The node
selects at most four eligible remote IPs per minute, preferring one endpoint
per IP using the shared endpoint ranking. It reserves connection capacity in
group-priority order before starting any selected pair, without backfilling
skipped selections. It opens nonblocking sockets from the fixed binding,
and allows ten seconds for TCP and twenty seconds for the complete encrypted
handshake and session-bound proof. This implementation uses one connect call
per selected pair, without application retries. On systems supporting a
listener on the shared binding, an accepted socket can complete a pending
attempt only when its local and remote endpoints match and its TCP deadline
has not elapsed. Other accepted sockets are closed. Kernel TCP retransmissions
still apply. Failure never falls back to another source port or v1 transport.

Authenticated connections appear as `holepunch` in `getpeerinfo`. Up to four
are allowed, separately from ordinary outbound peers, with at most one pending
or established holepunch connection per remote IPv4 address. They may perform
normal Bitcoin relay and survive advertisement expiry and observer loss. A clock
discontinuity cancels unfinished attempts. Full capacity cancels the remaining
attempts until a slot becomes available. Each started attempt sets a ten-minute
cooldown for its remote IPv4 address. Changing keys, ports, or epochs does not
reset it, and failure or disconnection does not extend it. The node retains at
most 64 unexpired cooldowns and skips attempts if no history slot is available.

The remote descriptor cache holds at most 1,024 records, with private keyed
placement into buckets of at most 16 records and limits of 32 records per
connected source network group and advertised destination network group.
Retained records cannot be displaced by new records before expiry. Separate
curve-validation budgets allow one record per second per peer (burst 32) and
16 per second across all peers (burst 256), including privileged peers and
invalid or conflicting keys. At most four new ECDH derivations are allowed per
round, both per source group and in total; derived keys are reused while their
descriptors remain valid. These limits can reject advertisements or skip
attempts before the overall connection or cache limit is reached.

For a controlled experiment, `-holepunchexternal=<IPv4>:<port>` supplies the
advertised endpoint directly and disables observer connections. This is an
explicit override of the draft's observation requirement. The advertised port
may differ from `-holepunchbind` when a NAT maps it to another external port.
Without fast mode it must still be a globally routable IPv4 endpoint, and the
draft's epoch and eligibility rules remain in effect.

`-holepunchfast` selects a separate test profile. All participating nodes and
address relays must enable it. This mode is restricted to regtest and custom
signet, and is incompatible with the draft profile:

| Parameter | Draft profile | Fast test profile |
| --- | --- | --- |
| Descriptor and proof format | `0x01` | `0xfd` |
| Cryptographic domain | `BitcoinHolePunch/v1` | `BitcoinHolePunch/test-fast/v1` |
| Round | 60 seconds | 30 seconds |
| Epoch | 3,600 seconds | 60 seconds |
| IPv4-pair eligibility per round | 1/32 | Always eligible |
| Start offset | 5,000–34,999 ms | 5,000–9,999 ms |
| Mean address send interval | 30 seconds | 1 second |
| Destination cooldown | 10 minutes | 30 seconds |
| Destinations | Global IPv4, different public IP | Unicast IPv4, including local addresses and same-IP different ports |

The fast profile uses feature identifier
`3e4f5bc6bad88ba8ec2be4c54dcdbef6f17b08884ad5ae1884ac2f4eabc547b0`.
Authentication, descriptor conflict rules, resource limits, and TCP/handshake
deadlines remain enforced. Startup waits for the next minute, and origination
waits five seconds into that minute. Candidates received during the first
round can be selected at second 30 and start at seconds 35–39.999. Failed
attempts wait for a later opportunity. Both profiles reject their retired
formats and feature identifiers without fallback.

Here is a three-process custom signet experiment on one machine. C relays
addresses; A and B advertise separate fixed loopback ports. Run from the
repository root after building `bitcoind` and `bitcoin-cli`:

```sh
mkdir -p /tmp/holepunch-C /tmp/holepunch-A /tmp/holepunch-B

build/bin/bitcoind -daemon -datadir=/tmp/holepunch-C \
  -signet -signetchallenge=51 -holepunchfast \
  -connect=0 -listen=1 -bind=127.0.0.1:38333 -port=38333 \
  -rpcport=38332 -listenonion=0 -natpmp=0

build/bin/bitcoind -daemon -datadir=/tmp/holepunch-A \
  -signet -signetchallenge=51 -holepunchfast -holepunch \
  -holepunchbind=127.0.0.1:38401 -holepunchexternal=127.0.0.1:38401 \
  -connect=127.0.0.1:38333 -listen=0 -rpcport=38342

build/bin/bitcoind -daemon -datadir=/tmp/holepunch-B \
  -signet -signetchallenge=51 -holepunchfast -holepunch \
  -holepunchbind=127.0.0.1:38402 -holepunchexternal=127.0.0.1:38402 \
  -connect=127.0.0.1:38333 -listen=0 -rpcport=38352

build/bin/bitcoin-cli -signet -datadir=/tmp/holepunch-A -rpcport=38342 getpeerinfo
build/bin/bitcoin-cli -signet -datadir=/tmp/holepunch-B -rpcport=38352 getpeerinfo
```

Both A and B should show a `holepunch` peer using transport `v2`, in addition
to their manual connection to C. They should have no `holepunch-observer`
connections. Stop each process using the corresponding `bitcoin-cli` command
with `stop` instead of `getpeerinfo` (C uses RPC port 38332).

A sparse cache does not increase the draft profile's eligibility probability.
With one mutually known candidate, opportunities occur every 32 minutes on
average before other failures. Every selected attempt then targets that one
candidate, so this protocol does not promise peer diversity. Fast mode makes
that lack of diversity explicit by making every pair eligible.

Observer-based participation requires peers in distinct globally routable IPv4
network groups and compatible NAT/socket behavior. Localhost nodes alone cannot
provide draft-compliant observations; manual fast mode supplies a local test
path. Unit tests exercise cryptography and lifecycle without contacting vector
addresses. Functional tests exercise address relay, configuration, and actual
three-node connection establishment, expiry survival, and block relay after
the intermediary disconnects. These checks do
not establish real NAT traversal success or the draft's ten-minute connection
target. Interoperability, shared-port behavior on each operating system, NAT
traversal, reciprocal discovery, and induced SYN traffic still require
measurements before public deployment.
