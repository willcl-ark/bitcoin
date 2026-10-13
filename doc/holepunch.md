# Experimental TCP hole punching

TCP hole punching lets two nodes behind compatible NATs establish a direct
IPv4 or IPv6 connection. It follows the [draft TCP hole punching BIP](https://github.com/0xB10C/bips/blob/17bbcc637049b9ce63a0faac91e0a9330481193f/bip-tcp-holepunch.md).
The feature is experimental and disabled by default. It requires v2 transport.

Enable `-holepunch=1` to participate as a connector and advertise eligible
endpoints. Enable `-holepunchcoordinator=1` to coordinate requests between peers.
Enable `-holepunchonion=1` to create dedicated Tor onion coordination services,
using the existing `-torcontrol` and Tor authentication settings. Both
coordination options imply `-holepunch=1`; explicitly disabling `-holepunch` or
v2 transport while enabling a role is an initialization error.

To run a regular coordinator from a built checkout:

```sh
./build/bin/bitcoind -daemon -listen=1 -holepunchcoordinator=1
```

Use your existing datadir and configuration options. You can also set
`holepunchcoordinator=1` in `bitcoin.conf`. The ordinary P2P port must accept
inbound connections, including through any firewall or NAT port forwarding.
Tor coordination uses Tor only for signalling; the resulting punched connection
uses direct IP.
There is no I2P coordination service implementation. The draft leaves its
specifics outside the scope of the protocol.

By default, the punch port is chosen uniformly at random from 49152 to 65535,
excluding bad ports. It remains fixed for the process lifetime, including across
network activity toggles. Set `-punchport=<port>` to choose a port between 1 and
65535 explicitly. Punch listeners operate independently of `-listen`. The
dedicated onion services operate independently of `-listenonion`
and share a separate loopback listener with an automatically assigned port.

IPv4 and IPv6 use separate coordination onion identities. Each is created after
two reporting peers agree on that family's external endpoint, and announced only
after Tor reports descriptor publication. An endpoint change stops announcements
under the old identity and requires a fresh one. Losing reporting quorum pauses
announcements while retaining the identity's endpoint association. Identities
also rotate after 24 hours; every retired service stays reachable for another
20 minutes to serve cached announcements.

Use `-punchbind=<addr>` for a stable local source address. It accepts a numeric
IPv4 or IPv6 address without a port, at most once per family. Without explicit
binds, punch listeners use wildcard addresses for eligible families. Disabled
networks and proxied address families do not gain direct clearnet connections.
NATs need consistent mappings, and eligible external endpoints require agreeing
reports from two connected reporting peers in distinct network groups.

Punch sockets require `SO_REUSEPORT`. Platforms without it, including Windows,
cannot enable this implementation. Startup, endpoint and attempt state changes
appear at the normal `LogInfo` logging level. `getpeerinfo` identifies punched
outbounds as `outbound-holepunch` and dedicated coordination connections as
`holepunch-coordination`.

For manual regtest experiments, give each node a distinct loopback IP address
and unique P2P and punch ports. Arrange enough reporting peers to establish each
endpoint. Regtest still requires two agreeing reporting peers. Under the regtest
waiver, each connected reporting peer may count independently, even if the peers
belong to the same network group; reports need not be globally routable.
