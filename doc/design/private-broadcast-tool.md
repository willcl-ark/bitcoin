# bitcoin-privbcast design notes

The goal is to broadcast a transaction without revealing the sender's IP address, onion
address, or long-term node identity.

`bitcoin-privbcast` sends one final transaction, or a parent and child, to a bounded set of
peers over Tor. A job makes at most 24 connections and finishes within ten minutes.
Only its report remains after it ends. `bitcoin-privbcast` is a standalone executable.
With `-privatebroadcast`, `bitcoind` does not launch that executable. RPC submissions queue
jobs in `bitcoind`, whose worker threads call the same job code directly instead of using the
node's connection manager.

Two rules govern each job. It uses no node state that a recipient could probe through P2P.
It also fixes its connection schedule before contacting a recipient, so a peer cannot change
when or where it connects next. The tool cannot guarantee delivery or hide the fact that a
job ran. It does not retry after the job ends.

## Who can observe a job

- A recipient sees a Tor exit or onion circuit, the tool's fixed wire profile, and the
  transaction. In package mode it can request the parent too. The job has no node IP or onion
  address, peer set, address manager, mempool, or validation cache to expose.
- A Tor exit on an exit-path connection sees the transaction and recipient. It can interfere
  with that connection. Onion connections do not use an exit.
- A DNS seed or the exit's resolver sees a query for the seed's name from a Tor exit. It can
  lie or tag its answers with addresses of peers it controls or colludes with. If the job
  selects one, the seed or resolver can learn the transaction and link it to the query.
- A network observer may see the transaction arrive at several peers within seconds and
  recognize the tool. That observation does not reveal where the sender connected from.
- The node's existing peers see the transaction only if it returns through the network, when
  the node relays it normally.

This does not hide that a job ran or which release profile it used. It also does not prevent
correlation with other traffic on the same Tor daemon. See "Limits".

## What changes from the old `-privatebroadcast`

The RPC entry points remain: `sendrawtransaction`, `getprivatebroadcastinfo`, and
`abortprivatebroadcast`. This change also adds `submitpackage`. The job behind them changes:

| | Before (in `CConnman` and `PeerManager`) | Now (a job) |
|---|---|---|
| Peers | from the node's address manager | discovered per job: the release DNS seeds resolved through Tor (`RESOLVE`), plus onion peers from the release fixed-seed list |
| Networks | Tor, I2P, IPv4/IPv6 through the proxy | Tor only: onion peers, and IPv4/IPv6 peers through Tor exits |
| Transport | v2 or v1 | v2 (BIP324) only |
| Connections at once | 3 per transaction when it is submitted (all private broadcasts share a cap of 64), each up to 3 min | 3 at start, never more than 6 (one per slot) |
| Connections in total | more with every re-send | at most 24: 6 slots, each a first peer and up to 3 backups |
| Retries | re-sent to new peers until seen back in the node's mempool (after 1 min), up to 1,000 times | none after an announcement; the schedule is drawn at job start and nothing seen on the network changes it |
| Duration | open-ended | every job's network work ends within 568 s |
| Peer profile | `NODE_NONE`, no wtxid relay, announces by txid | `NODE_WITNESS`, protocol 70017, requires wtxid relay (BIP339) and announces by wtxid |
| Packages | no | one parent and its child |
| Without a node | no | the `bitcoin-privbcast` program |

The new job gives up I2P, v1-only peers, the node's address manager, and re-sending when the
transaction does not come back. Its network work has a fixed limit: at most 24 connections,
complete within 568 s, with every attempt recorded in a report.

A hostile recipient learns no more about the sender's IP address or long-term node identity
than an honest recipient. Random peers and separate Tor paths help delivery. A peer that drops
the transaction, or an exit that interferes, can cost one slot without stopping the others.

## The two rules

1. **A job exposes no node state.** It has no address manager, ban or discouragement list,
   connection table shared with ordinary peers, upload accounting, or validation caches.
   Misbehavior by one recipient ends only that connection. When the tool exits, only its
   report remains. A companion change to `testmempoolaccept` keeps the recommended preflight
   check from changing the node's validation and coins caches. Reading the inputs still warms
   its database and page caches, as any UTXO lookup would.

2. **Peer behavior cannot change the schedule.** At job start the tool draws the time and
   hard lifetime limit for every connection opportunity. After discovery, it assigns all
   candidate peers before contacting any recipient. A failure before announcement permits
   only that slot's next assigned attempt, at its original time. An announcement cancels the
   remaining attempts in that slot. Protocol replies on an active connection follow fixed
   rules. If an attempt cannot start within 5 s of its scheduled time, the tool skips it.
   Cancellation ends the whole job. Seeing the transaction return through the network
   changes nothing.

## What a recipient sees

Each connection uses the same wire profile. The tool sends VERSION with constant fields:
protocol 70017, `NODE_WITNESS`, no relay, no height, no time, and user agent
`/pynode:0.0.1/`. It answers the peer's VERSION with WTXIDRELAY and VERACK, announces the
transaction by wtxid, and serves it once if asked for that wtxid. It then sends one PING and
closes when the matching PONG arrives.

Protocol 70017 is Core's current version, so this field follows the release. The user agent
is the constant used by the old implementation instead of the node's own user agent. Keeping
it avoids introducing another profile; see bitcoin/bitcoin#27509.

The peer must use protocol 70016 or later, offer `NODE_WITNESS`, accept relay, and send
WTXIDRELAY before VERACK. Otherwise the tool disconnects before announcing anything. It reads
and ignores all other messages. If the peer declines the transaction, never asks for it, or
never answers PING, the connection ends at its fixed deadline instead of ending earlier.

## The schedule

All times are measured from job start.

- **Discovery, 0-18 s.** The tool asks Tor to resolve each release DNS seed four times, using
  a separate Tor stream for each query. It keeps up to three answers per seed and adds up to
  eight onion peers from the release fixed-seed list. Queries stop at 15 s. Delivery starts
  at 18 s even if some answers are late or missing.
- **Six delivery slots.** Each slot has a first peer and up to three assigned backups. Three
  slots open together at 18 s: two through Tor exits and one to an onion peer. The other
  three start at times drawn when the job begins. One onion slot starts 35-180 s after
  delivery begins, at 53-198 s from job start. Two exit-path slots start 185-240 s after
  delivery begins, at 203-258 s from job start, at least 5 s apart. If the onion list runs
  out, an onion slot uses an exit-path peer instead. Exit-path slots never use onion peers.
- **Each connection.** From its scheduled time, an attempt has 45 s to connect, complete
  the encrypted handshake, and announce. The peer then has 75 s to ask for the transaction,
  followed by 10 s to answer PING after the tool writes it. If an attempt fails before
  announcement, the slot may try its next assigned peer 50-60 s after the previous scheduled
  time. That interval is drawn at job start. It can make at most three backup attempts.
  Failure includes a peer closing the encrypted handshake because it only speaks v1; there
  is no v1 fallback. Once a slot announces, it makes no more attempts. Slots do not wait
  for each other.
- **End.** A slot finishes network work within 310 s of its first scheduled time. The last
  slot therefore finishes by 568 s. A hard ten-minute cap also applies. Host scheduling
  may make a step late, but it cannot move a scheduled time.

All durations are compile-time constants in `src/privbcast/*.h`. Changing them per user would
make those users distinguishable, so there are no settings for them.

The timing choices have different sources. Per-connection budgets determine the 50 s backup
floor, 310 s slot limit, and 568 s job limit. The 18 s discovery window and 30 s parent hold
come from measurements: RESOLVE bursts finished within 8 s in nine of ten runs on one Tor
client, and signet peers requested an orphan's parent within about 4 s. The onion reachability
figure in "Limits" comes from probing each release's fixed-seed list. The six slots, three
prompt slots, three backups, and two later windows are design choices to keep the job small
while tolerating failed paths and peers. None is a per-user privacy setting.

Hiding the sender's address depends on avoiding node state and using Tor. The schedule helps
with delivery and keeps peers from steering later attempts. Its later slots also start at
random offsets within fixed windows, in a fixed order and at least 5 s apart. That may make
jobs less regular, but it is not a privacy guarantee. The three prompt slots open together,
and backups can still cluster.

## One parent, one child

A child can pay for a parent whose fee is too low for mempool acceptance on its own, if the
recipient evaluates them together. Bitcoin Core 28 and later can do this for one parent and
one child. Give the tool both transactions in either order; it identifies their relationship.

Only the child is announced. Announcing the parent would invite a request for it first. A
low-fee parent received alone would be rejected, and the tool does not serve a transaction
twice.

The tool serves the child once, in response to an exact, single-entry request by wtxid. It
then waits up to 30 s for a parent request. On signet, a recipient missing the parent asked
for it about 4 s after receiving the child. A parent request may include other inputs of the
child. The tool serves the parent and replies `notfound` for entries it does not have. It
never sends `notfound` for either of its own transactions.

The parent can be served once, and only in these cases:

- After serving the child, the tool receives a request for the parent by txid as part of
  orphan resolution.
- Before serving the child, the tool receives a request for the parent without the child.
  This can happen when the recipient already has the child as an orphan from another peer.
  Our announcement adds the tool as an announcer of that orphan, so the recipient asks us
  for the parent.

Before the child has been served, the tool ignores a request that names both child and
parent, without sending `notfound`. The recipient cannot have learned the child's inputs
from us at that point, so our announcement did not cause that request. The connection stays
open, and the tool can still answer a later request for the child. Once it serves the parent,
it sends PING and serves nothing else on that connection.

If no parent request arrives during the 30 s hold, the tool sends PING anyway. It cannot
know whether the peer already has the parent, is waiting for another peer, or will reject
the package. The same request window bounds the full exchange. A second request does not
restart it, and package mode does not change announcement timing or backup rules.

Acceptance depends on the parent and the recipient's version. Bitcoin Core 28 and later
accept a parent below the minimum relay feerate as part of a package only if it is TRUC
(version 3). A non-TRUC parent below that feerate needs Bitcoin Core 31 or later; older
recipients drop it. Whether the package reaches miners depends on the software they run.

The tool cannot check whether the child has another unconfirmed parent or pays enough for
both transactions. There is no package dry run: `testmempoolaccept` evaluates each
transaction on its own. It reports "min relay fee not met" for a low-fee parent even if the
package could be accepted, then stops without checking the child. That rejection tells you
nothing about the child's validity. Calculate the package feerate yourself. The tool also
cannot tell whether a recipient accepted the package; PONG means only that it processed
what we sent. A recipient older than Core 28 requests the parent, rejects it alone, and
holds the child as an orphan only until we disconnect.

The tool serves a second transaction only in package mode, when given two transactions or
called through `submitpackage` under `-privatebroadcast`. A recipient that requests the
parent can therefore tell the sender used package mode. The relationship between the two
transactions is already visible on-chain.

## Using it

1. Check the transaction with `bitcoin-cli testmempoolaccept`. The tool itself does only
   stateless sanity checks. The preflight leaves the node's validation and coins caches
   unchanged, though input reads warm its database and page caches as any UTXO lookup does.
   If that matters, use a node that is not public. For a parent and child, the check rejects
   a low-fee parent on its own; see "One parent, one child".
2. Feed the final hex on stdin: `bitcoin-privbcast send < tx.hex`. By default the tool uses
   Tor at 127.0.0.1:9050; use `-tor=` for another listener. The JSON report goes to stdout
   and progress lines go to stderr.
3. To check whether the transaction reached your node, use `bitcoin-cli getmempoolentry`.
   Do not also broadcast it through the ordinary path.

`send` exits with status 0 if at least one announcement was fully written, 2 if none was,
and 1 for bad input or arguments. The JSON report has no wall-clock value and records the
job. Progress lines on stderr are best effort: if a pipe cannot take a line, the tool drops
it without waiting. See `-help`. Both the report and progress lines are local evidence of
a broadcast, so store them as carefully as a wallet log.

The proxy must run on this machine. A remote proxy would expose destinations and credentials
in transit. It must accept SOCKS authentication, Tor's RESOLVE extension, and IPv6. The tool
cannot verify that a proxy is Tor. A different proxy is unlikely to deliver anything: each
stream uses fresh username/password credentials, which `ssh -D` and most VPN clients do not
support; exit-path candidates come only from RESOLVE answers; and the bundled list contains
only onion addresses. Such a proxy learns that a job ran, but not which transaction it
carried.

When the node and job share a SocksPort, circuit separation requires
`IsolateSOCKSAuth`, Tor's default. Fresh credentials alone cannot enforce it. With
`NoIsolateSOCKSAuth`, Tor may put several job streams on one circuit. One exit could then
see the transaction reach several recipients. Tor may also share a circuit with other
traffic through that SocksPort, including the node's own connections and anything they
reveal, such as its advertised onion address. The tool cannot detect this through SOCKS:
Tor accepts the credentials either way. The operator must ensure the SocksPort uses
stream isolation.

## Inside the node

With `-privatebroadcast`, `sendrawtransaction` and `submitpackage` queue jobs in
`bitcoind`'s `PrivateBroadcastManager`. A worker calls `privbcast::RunJob()` in the
`bitcoind` process. The standalone executable calls that same function when used from the
command line. A job does not use the node's address manager, connection manager, peer manager,
or ban list. It uses the tool's discovery, schedule, and wire profile.
The transaction enters the node's mempool only if it returns through the network; the node
then handles it normally. Submitting it again queues another job, even if the mempool
already holds it. Jobs are not deduplicated.

Node peer settings do not apply to these jobs: `-onlynet`, `-dnsseed`, `-fixedseeds`,
`-connect`, `-seednode`, and `-addnode`. Even with `-onlynet=onion`, a job can connect through
Tor exits. Fixed-seed onion addresses age with the release, while Tor hides the user's
address on either path. An exit can interfere only with its own connection, which cannot
control the other slots. There is no setting to change this behavior.

A job does share a few things with the node:

- **Proxy.** It uses the node's Tor proxy. The proxy and the path to it are trusted, as with
  the node's other proxy settings: SOCKS carries destinations and credentials in clear. Every
  stream gets fresh credentials regardless of `-proxyrandomize`.
- **Queue.** At most two jobs run at once, in submission order. A job ends when its last
  connection ends, so a recipient that holds a connection open can delay the next queued
  job. That can link two of this node's transactions, though it does not identify the node.
- **Mempool observation.** The report records when the node's mempool first sees the
  transaction. The observation never changes the job.
- **Log.** `debug.log` records job progress with `-debug=privatebroadcast`. SOCKS failures
  that name a destination appear only with `-debug=proxy` or `-debug=net`.
- **Process.** Shutdown or `setnetworkactive false` cancels jobs.

Wallet sends do not use private broadcast. The wallet submits transactions to the node's
mempool and rebroadcasts them from there. Supporting private wallet sends requires a
separate change.

## Limits

- The tool and node still share a host and usually a Tor daemon. They share load, Tor's
  caches, and the client's guard. This design does not separate them.
- DNS seeds, resolvers, and Tor exits are untrusted. A seed or resolver can lie or tag its
  answers to steer the job toward peers it controls or colludes with. An exit can also act as
  the peer on an exit-path connection, since the encrypted transport does not authenticate
  the peer. Onion connections do not pass through an exit.
- Onion peers come only from the release's fixed-seed list, which ages. A year after a list
  is generated, roughly one onion in six still accepts the encrypted transport. On an old
  release, most onion slots fall back to exit-path peers, so the job has fewer paths without
  an exit.
- Recipients can recognize and probe the release by its wire profile. They learn that
  someone used the tool, not who used it.
- Discovery does not exclude the node's own addresses. Doing so would require node state,
  contrary to the first rule. If a job picks the node's own onion or IP address, the node
  becomes one of the job's first relayers.
- Tor's timing and reachability vary between users. The schedule fixes when the tool acts,
  not when the network responds.
- The job ends with its schedule and never retries on its own. Retrying because the
  transaction did not return would let a network adversary influence the job. To censor
  one, an adversary must prevent every slot from delivering. A recipient can announce
  successfully and then withhold the transaction; that slot will not try its backups. If
  `getmempoolentry` shows no transaction, the user can submit another job. It will discover
  peers and draw a new schedule.
