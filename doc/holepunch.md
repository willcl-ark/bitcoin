# Experimental TCP holepunching

`-holepunch` opts into the coordinator-less TCP holepunch protocol described in
[the draft](https://delvingbitcoin.org/t/2497/22). It requires `-v2transport`.
The protocol uses dedicated connections to announcement boards and attempts a
connection when a new block selects a mutually known endpoint. A successful
connection uses BIP324 v2 transport, without v1 fallback.

A participant opens two board connections per address family, using boards in
its address manager that advertise the experimental board service bit. Boards
must agree on its external endpoint before it announces. This requires a NAT
whose mapping does not depend on the destination. Ordinary outbound connections
continue to use their usual source ports and connection limits. Board connections
and punch attempts use direct IPv4 or IPv6 sockets; normal proxy settings do not
route these connections.

`-holepunchboard` opts into operating an announcement board. A board must be
reachable on its ordinary P2P listening port. It advertises experimental service
bit 24 and accepts at most 32 dedicated visitors, with one visitor per IP address
and four per network group. Board connections carry verified announcements and
keepalives. Their announcements are held in memory and never enter the address
manager or ordinary address gossip.

`-holepunchport=<port>` and `-holepunchportv6=<port>` select the IPv4 and IPv6
source and listening ports. Each defaults to a random port in 49152-65535 that
stays fixed until shutdown. `-holepunchbind=<address>` selects a stable local
address and may be specified once per family. For IPv6, use an address that does
not rotate while the node is running. The operating system must support sharing
the source port between the listener and outgoing TCP sockets.

The initiator of a successful punch uses one additional outbound slot. The
responder uses an ordinary inbound slot. Pending sessions reserve their slot;
board connections have a separate pool. A participant closes its board
connections while neither kind of slot is free.

A registration is acknowledged only when its board echoes the exact current
endpoint and key within ten seconds. Unrelated announcements do not acknowledge
or reject it. To preserve capacity under frequent visitor updates, each board
recipient has separate hourly allowances for its initial roster and periodic
refreshes, self acknowledgements, and coalesced updates. Repeated updates from a
visitor replace its pending update without moving ahead of other visitors. The
combined allowance is at most 240 elements per hour.

On regtest, local endpoints are permitted and peer endpoints form separate
network groups, so multiple nodes on one host can exercise the protocol. Give
participants distinct local addresses to preserve the rule against attempting
a node's own external address. The existing regtest `addconnection` RPC accepts
`holepunch-board` with v2 transport for explicitly connecting test visitors to
boards whose local addresses cannot enter the address manager.
