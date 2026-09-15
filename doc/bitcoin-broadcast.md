# Sending a transaction through Tor

`bitcoin-broadcast` hands one raw transaction to an explicitly selected Bitcoin onion
peer, without running a node or using wallet data, RPC, configuration files, or a
data directory. Create and sign the transaction separately. Recipient selection
and confirmation tracking are the caller's responsibility.

## Prerequisites

Run a trusted local Tor daemon with a loopback SOCKS port. The sender uses Tor's
default SOCKS-auth stream isolation; no additional Tor configuration is normally
required. For example, the Tor configuration can contain:

```text
SocksPort 127.0.0.1:9050
```

The sender requires SOCKS5 username/password authentication and uses Tor's
recommended username `<torS0X>0` with a fresh random password per connection,
including transport fallback. Do not disable isolation with `NoIsolateSOCKSAuth`.
Authentication success cannot verify Tor's configuration or prove circuit
isolation. The tool does not start Tor or inspect its control port.

Use valid Tor v3 onion destinations supplied by a trusted source. No address
discovery or DNS lookup is performed. Every network connection targets the
configured local SOCKS endpoint, which receives the onion as a domain name.

## Finding onion peers

If you have a running Bitcoin Core node, ask it for known onion addresses:

```sh
bitcoin-cli -named getnodeaddresses count=8 network=onion
```

With `jq` installed, print them as destinations ready to copy:

```sh
bitcoin-cli -named getnodeaddresses count=8 network=onion \
  | jq -r '.[] | "\(.address):\(.port)"'
```

Use `count=1` for one candidate. Choose one or a few printed destinations for
the usage example below, retaining their ports. Use the node's usual
`bitcoin-cli` options for its data directory and chain; for example, use
`-signet` with the CLI and `-chain=signet` with the broadcast tool.

The RPC filters known addresses for quality and recency, but does not restrict
them to the address manager's `tried` table. It does not guarantee that a peer
is reachable or honest. If it returns an empty array, the node has no qualifying
known onion addresses; obtain an address from another trusted source instead.
This query does not broadcast the transaction. It reads the node's in-memory
address manager, so there is no need to wait for a `peers.dat` flush.

## Usage

```sh
bitcoin-broadcast [options] <onion-host[:port]> [<onion-host[:port]> ...]
```

Replace the placeholders with your chosen destinations:

```sh
peer1='ONION_HOST_1:PORT_1'
peer2='ONION_HOST_2:PORT_2'
bitcoin-broadcast -proxy=127.0.0.1:9050 "$peer1" "$peer2" < signed-tx.hex
```

For a single destination, omit `"$peer2"`.

In Bash, pass the RPC results directly as destination arguments:

```bash
bitcoin-broadcast $(bitcoin-cli -named getnodeaddresses count=8 network=onion | jq -r '.[] | "\(.address):\(.port)"') < signed-tx.hex
```

The unquoted command substitution deliberately splits the printed destinations
into separate arguments. The transaction still comes from the file. These are
fallback candidates, not a request to broadcast to every peer: the tool stops
after successful handoff or possible disclosure. Use `count=1` for one candidate.

Read one hexadecimal raw transaction from stdin to EOF, including witness data
when present. A single final LF or CRLF is optional. Do not supply the transaction
in process arguments. Whitespace, extra records, trailing garbage, coinbase
transactions, and oversized or structurally invalid transactions are rejected
before connecting. All options and destinations are validated before connecting,
even destinations later in the list. Duplicate host/port pairs are removed while
preserving the original order.

| Option | Meaning |
| --- | --- |
| `-proxy=<ip:port>` | Numeric loopback SOCKS endpoint with an explicit port, default `127.0.0.1:9050`. Bracket IPv6, for example `[::1]:9050`. Remote proxies, wildcard addresses, hostnames, and Unix sockets are not accepted. |
| `-chain=<chain>` | Connection network, default `main`. Supports Core's standard `main`, `test`, `testnet4`, `signet`, and `regtest` chains. An omitted peer port uses that chain's default P2P port. |
| `-timeout=<seconds>` | Positive integer total deadline per physical connection attempt, default `60`. Covers proxy connection through the final pong. Incoming traffic cannot extend it. |
| `-help`, `-version` | Print help or version and exit successfully without reading stdin or connecting. |

Options precede destinations. The tool does not read `bitcoin.conf`,
`settings.json`, `peers.dat`, or wallet files, and does not create files.

## Handoff and exit status

The sender prefers BIP324 v2 transport. A v1-only peer may receive one reconnect
to the same onion through a new authenticated SOCKS connection, using Core's
existing fallback conditions. That reconnect gets a fresh deadline. Partial v2
replies and post-handshake failures do not permit transport fallback.

A recipient must advertise protocol version at least 70016, witness support,
block-serving services, and transaction relay. The sender announces one txid,
waits for an exact request for that transaction, sends its full witness-preserving
serialization, and sends a fresh ping.

- Status `0` means the complete transaction and subsequent ping were written,
  and a matching pong was received. Both stdout and stderr are empty.
- Status `1` means handoff was not confirmed. A concise reason appears on stderr.
  Failures before possible disclosure may advance to the next destination.
  Local proxy or authentication failures stop the invocation.
- Once the first announcement write is attempted, no failure permits another
  destination or reconnect, even if the write failed or only partly completed.
  The diagnostic warns: `Transaction information may have been disclosed;
  handoff not confirmed; no further peers tried.`

Status `0` does not prove mempool acceptance, wider propagation, or eventual
confirmation. A dishonest peer can request and discard the transaction and still
answer the ping. Track confirmation independently. An already-known transaction
may not be requested, so a repeated invocation can fail after announcement.

With `N` unique destinations and timeout `T`, network work is bounded by `2*N*T`,
apart from bounded local processing and cleanup. Stdin reading happens before the
network deadlines. Sessions also limit received wire bytes to 8 MiB and completed
messages to 1024. Interrupting after possible disclosure retains the warning.

## Limits and privacy

Input checks cannot verify signatures, input availability, fees, confirmation, or
mempool policy. A raw transaction has no chain tag. Chain selection determines
network magic and ports, not whether the transaction belongs to that chain.

The fixed version profile contains no local addresses, wall-clock time, tip
height, or software build identifier. Session keys, SOCKS passwords, and version
and ping nonces are fresh; no peer history or session state is persisted.
Diagnostics do not print transactions, hashes, onion destinations, credentials,
or peer-provided strings.

Trust the local host, Tor daemon, and Tor configuration. Tor protects the onion
connection even with v1 transport. BIP324 adds encryption but does not authenticate
the Bitcoin peer's identity. Neither transport proves that a recipient is honest.
The recipient sees the transaction. This tool does not prevent on-chain linkage
or defeat a sufficiently capable timing observer. Its fixed handshake and short
session can identify broadcasting clients as a class.

Repeated manual invocations can disclose the same transaction to more peers.
Avoid other broadcasting paths first: `sendrawtransaction` already broadcasts.
The tool does not retry later or rebroadcast automatically. The node's built-in
`-privatebroadcast` option and its `getprivatebroadcastinfo` and
`abortprivatebroadcast` RPCs have been removed. Wallet and `sendrawtransaction`
broadcasts use ordinary node relay; they do not invoke `bitcoin-broadcast`.

## Building

Enable `BUILD_BROADCAST` explicitly for standalone or release builds. Its default
matches `BUILD_TESTS`. Building just the target avoids unrelated node/test targets:

```sh
cmake -S . -B build-broadcast -DBUILD_BROADCAST=ON
cmake --build build-broadcast --target bitcoin-broadcast
cmake --install build-broadcast --component bitcoin-broadcast
```

The binary links shared transport, common, and utility code, not `bitcoin_node`.
The installation component includes this guide and, with `INSTALL_MAN=ON`, the
manpage. Follow `contrib/devtools/README.md` to generate release manpages from help.
