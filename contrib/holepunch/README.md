# Bitcoin Core TCP hole-punching sidecar

This directory contains an experimental TCP hole-punching rendezvous server and
raw proxy sidecar. It is intended for manual testing only.

The sidecar binds a fresh TCP port for each rendezvous registration by default.
Bitcoin Core keeps owning its normal P2P port, for example `8333`.

## Topology

```text
initiator bitcoind -> temporary initiator sidecar port
initiator sidecar  -> punched TCP tunnel -> receiver sidecar rendezvous port
receiver sidecar   -> receiver bitcoind :8333
```

The coordinator assigns one side as the initiator and one side as the receiver.
This is needed because Bitcoin Core sends its initial `version` message for
outbound connections. If both sidecars simply connected into their local Core
nodes, both Core nodes would see inbound connections and the raw proxy would
not start the Bitcoin P2P handshake.

## Running

Run the coordinator on a publicly reachable host:

```bash
python3 contrib/holepunch/server.py 57996
```

Run Bitcoin Core normally on both peers, keeping port `8333` for Core:

```bash
bitcoind -listen=1
```

Run the sidecar on both peers:

```bash
./contrib/holepunch/sidecar.py
```

By default, the sidecar connects to the coordinator at
`116.202.23.118:57996`, binds a fresh rendezvous port for each registration,
and proxies to local bitcoind at `127.0.0.1:8333`. Use `--bitcoind-host` if
bitcoind listens somewhere else.

On the side that the coordinator logs as `role=initiator`, the sidecar tries
to make Core open an outbound connection to a temporary loopback listener by
running a command like:

```bash
bitcoin-cli addnode 127.0.0.1:<temporary-port> onetry
```

By default, `bitcoin-cli` is resolved from `$PATH`.

If `bitcoin-cli` needs extra arguments, pass the command prefix as one shell
string:

```bash
./contrib/holepunch/sidecar.py --cli-command="bitcoin-cli -datadir=<path>"
```

If the automatic command fails, run the printed `bitcoin-cli addnode` command
manually.

The initiator sidecar consumes that local Core connection and proxies it over
the punched TCP tunnel. The receiver sidecar connects the tunnel to its local
Core listener at `127.0.0.1:8333`. After a match is established, the sidecar can
register again with a new rendezvous port while the existing proxy session
continues running, until it reaches `--target-peers`.

Each registration also sends the coordinator the public IP addresses of peers
with pending or active proxy sessions. The coordinator avoids pairing clients
when either side has excluded the other's observed IP address, so a sidecar
does not open multiple connections to the same peer IP.

## Limitations

- This is not production-ready.
- NAT behavior varies; symmetric NATs may fail.
- The coordinator learns which sidecars are online and pairs them.
- The coordinator learns which peer IPs each sidecar is already connected to.
- The sidecar starts raw proxying after the punched TCP connection is
  established.
- The sidecar relies on `SO_REUSEPORT` so it can listen on the rendezvous port
  and also open outbound TCP connections from that port.
- The sidecar currently uses IPv4 sockets for outgoing punch attempts.
- Core sees the sidecar-local address, not the remote node's public address.
