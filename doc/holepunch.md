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

## Trying it on signet

The minimal participant command with holepunch logging is:

```bash
./build/bin/bitcoind -signet -debug=holepunch -holepunch=1
```

Add `-prune=550` if desired. V2 transport is enabled by default, and the node
automatically selects a random punch port per address family, fixed until
shutdown. `-holepunchport` is an optional override. The more explicit commands
below constrain the test to IPv4 and use a known port for firewall setup and
diagnostics. These are test choices, not requirements for participation.

Use this branch on every node. For an IPv4 test, arrange two publicly reachable
boards in different network groups and two participants on different public IPs.
Without `-asmap`, IPv4 groups are /16s; with it, choose different mapped ASNs.
One person can operate both boards. Sync the participants on default signet
before enabling holepunching: `bitcoin-cli -signet getblockchaininfo` should show
`"initialblockdownload": false`.

The two roles can run together, but three nodes using only one another as boards
will not demonstrate a punched connection. Each already has a board connection
to the other participants' punch endpoints, and rule R-7.3 excludes endpoints
with an existing connection.

Three people can instead each run a separate board daemon alongside their
participant daemon. The commands below give the board its own data directory
and RPC port, leaving the participant on the defaults. Each participant must
use the other two hosts' boards. All three public IPv4 addresses must then be
in different network groups. This avoids connecting through one's own public
address and keeps board visitor connections out of the participant's peer table.

On each board host, replace `BOARD_PUBLIC_IPV4` with that host's public address:

```bash
mkdir -p "$HOME/.bitcoin-holepunch-board"
./build/bin/bitcoind -signet -daemon \
  -datadir="$HOME/.bitcoin-holepunch-board" -rpcport=38335 \
  -holepunchboard=1 -v2transport=1 -onlynet=ipv4 -listen=1 \
  -port=38333 -externalip=BOARD_PUBLIC_IPV4:38333 \
  -debug=holepunch -logips=1 -logtimemicros=1
```

Allow incoming TCP on port 38333, forwarding it if the board is behind a router.
The RPC port does not need to be publicly reachable. To control this board,
include `-datadir="$HOME/.bitcoin-holepunch-board" -rpcport=38335` in its
`bitcoin-cli -signet` commands.

On each participant, after stopping its existing signet node:

```bash
./build/bin/bitcoind -signet -daemon \
  -holepunch=1 -v2transport=1 -onlynet=ipv4 -listen=0 -natpmp=0 \
  -holepunchport=54321 \
  -debug=holepunch -logips=1 -logtimemicros=1
```

Participants can all use port 54321 on their respective hosts. `-listen=0`
disables the ordinary P2P listener; the punch listener remains active. Keep the
default connection limits so the responder has capacity. Allow the punch port
through the host firewall, but leave it without a router port forward when
testing NAT traversal. If needed on a multihomed host, add
`-holepunchbind=LOCAL_LAN_IPV4`. The two boards must observe the same external
IP and port for a participant; destination-dependent NAT mappings cannot form
the required quorum.

### Learning the boards

Board discovery uses service bits in the address manager. There is currently no
signet RPC to open a board connection directly. On each participant, use this
Bash helper (requires `jq`) to learn each board's services through a temporary
ordinary connection, then release that connection for automatic board discovery:

```bash
learn_board() {
  local ip="$1" endpoint="$1:38333" peer attempt
  ./build/bin/bitcoin-cli -signet addpeeraddress "$ip" 38333 || return
  ./build/bin/bitcoin-cli -signet addnode "$endpoint" onetry true || return
  for ((attempt = 0; attempt < 60; attempt++)); do
    if peer=$(./build/bin/bitcoin-cli -signet getpeerinfo | jq -ce \
      --arg addr "$endpoint" '.[] | select(.addr == $addr and
        (.servicesnames | index("HOLEPUNCH_BOARD")) and
        (.servicesnames | index("P2P_V2")) and .pingtime != null)'); then
      if [[ $(jq -r '.connection_type' <<< "$peer") != holepunch-board ]]; then
        ./build/bin/bitcoin-cli -signet -named disconnectnode \
          nodeid="$(jq -r '.id' <<< "$peer")" || return
      fi
      return 0
    fi
    sleep 1
  done
  printf 'No completed board handshake with %s\n' "$endpoint" >&2
  return 1
}

learn_board BOARD_1_PUBLIC_IPV4
learn_board BOARD_2_PUBLIC_IPV4
```

Do not leave persistent `-addnode` connections to these endpoints: ordinary
connections prevent the dedicated board connections. Within a few seconds of
disconnecting the temporary peers, check:

```bash
./build/bin/bitcoin-cli -signet getpeerinfo | jq '
  [.[] | select(.connection_type == "holepunch-board" and .inbound == false) |
    {id, addr, addrlocal, transport_protocol_type}]'
```

Expect two outbound board connections using v2, with matching `addrlocal`
endpoints. Wait for exact registration acknowledgements in the participants'
logs, then at least two new signet blocks for announcement maturity. A punch is
not guaranteed on every block: selection, connection diversity and NAT behavior
can prevent it. A failed session may need a later board refresh before retrying.

### Watching progress

```bash
tail -F ~/.bitcoin/signet/debug.log | rg --line-buffered '\[holepunch\]'
```

The board command above writes to
`~/.bitcoin-holepunch-board/signet/debug.log`. Adjust paths if using other data
directories. `-debug=holepunch` logs board handshakes, reported endpoints,
quorum changes, registration acknowledgements,
candidate counts, selected sessions, TCP attempts and retries, and v2 handshake
results. `-logips=1` includes addresses for diagnosing mappings; omit it to keep
endpoints redacted in these debug messages. On a running node built with this
branch, enable the category with `bitcoin-cli -signet logging '["holepunch"]'`.

Successful punches log `Punched v2 handshake complete`. In `getpeerinfo`, the
initiator has `connection_type: "holepunch"`; the responder is an ordinary
`"inbound"` peer. Both show `transport_protocol_type: "v2"` and use their local
punch ports. Board connections alone do not demonstrate successful NAT traversal.
