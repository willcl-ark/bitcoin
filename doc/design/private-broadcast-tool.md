# bitcoin-privbcast: design notes

The goal is one thing: broadcast a transaction without ever revealing the sender's permanent
identity, its IP address or its onion address. Everything else here serves that.

`bitcoin-privbcast` announces one final transaction, or one parent and its child, to a small,
bounded set of peers over Tor, and then stops: at most 24 connections, all over within ten
minutes, nothing kept afterwards. It runs as a separate program, and the same code runs inside
`bitcoind` as `-privatebroadcast`, replacing the connection-manager-based implementation. Two
rules make it. A job touches no node state, so nothing a recipient sees at the P2P layer can be
tied to the node. Its schedule is drawn before the first connection and never moved by anything
a peer does, so nothing the network does can steer it. The tool does not promise delivery, does
not hide that a job ran, and does not retry on its own.

## Who sees what

Every party sees a Tor circuit and never the sender.

- **A recipient**: an exit or an onion circuit, a constant wire profile, and the transaction (a
  child and, on request, its parent). No IP, onion address, peer set, address manager, mempool
  or validation cache exists on the job's side to leak.
- **A Tor exit**, on an exit-path connection: the transaction and the recipient. It can drop or
  alter that one connection; it is never on an onion slot's path.
- **A DNS seed, or an exit's resolver**: a query for the seed's name from a Tor exit. It can
  answer falsely; it learns that a job ran, not which transaction.
- **A network observer**: the transaction appearing at several peers within seconds, so that
  the tool was used, and nothing about where from.
- **The node's own peers**: the transaction only once it has come back from the network,
  relayed like any other.

Not claimed: that a job ran at all, that it ran from a release with this profile, and anything
that correlates the job with other traffic through the same Tor daemon (see Limits).

## Compared with `-privatebroadcast` before this change

The entry points stay (`sendrawtransaction`, now also `submitpackage`, and the
`getprivatebroadcastinfo` and `abortprivatebroadcast` RPCs); what runs behind them is new.

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

Read down the right column, this is a smaller feature: no I2P, no peers that speak only the
old transport, no reach into the node's address manager, and no re-sending when the transaction
does not come back. Each is given up for the two rules below, and for a bounded cost with a
clear outcome: at most 24 connections, over within 568 s, and a report of every attempt.

**Delivery depends on recipients; privacy does not.** A recipient that turns out to be hostile
learns nothing more about the sender's IP address or long-term identity than an honest one does.
Choosing recipients at random, across three paths, is for robustness: a recipient that drops the
transaction, or an exit that interferes with it, costs one slot, not the job.

## The principle

Two rules, both enforced by construction rather than by careful coding:

1. **The tool creates no state a node can be read through.** It has no address manager,
   no ban or discouragement list, no connection table shared with ordinary peers, no
   upload accounting and no validation caches. A recipient that misbehaves ends only its
   own connection. When the tool exits, nothing it did persists anywhere but in its own
   report. The companion change to `testmempoolaccept` keeps the recommended preflight
   from leaving a trace in the node's validation caches and coins cache; reading the
   inputs still warms the database and page caches, like any UTXO lookup does.

2. **The schedule is drawn when the job starts, and nothing observed on a connection moves
   it.** At job start the tool draws every connection opportunity's time and hard lifetime
   limit. After discovery it assigns every candidate before contacting any recipient. Nothing
   received from a recipient reschedules or reassigns an opportunity. A failure before we
   announced enables only the same slot's next pre-assigned opportunity, at its scheduled
   time; an announcement suppresses that slot's remaining opportunities. Protocol responses
   follow fixed rules on the current connection. An opportunity not started within 5 s of its
   time is skipped rather than shifted, and cancellation stops the whole job. The tool never
   reacts to a transaction being seen back on the network.

## What a recipient sees

One connection, one fixed profile. The tool sends a VERSION with constant fields (protocol
70017, `NODE_WITNESS`, no relay, no height, no time, user agent `/pynode:0.0.1/`), answers the
peer's VERSION with WTXIDRELAY and VERACK, announces the transaction by wtxid, serves it
exactly once when asked for it by wtxid, sends one PING, and closes on the matching PONG. The
protocol version is Core's current one, so the profile tracks the release rather than marking
the tool; the user agent is the one the previous implementation sent (a constant other than the
node's own, see bitcoin/bitcoin#27509), kept so this change adds no second profile. It
needs a peer that is at protocol 70016 or later, offers `NODE_WITNESS`, accepts relay and sends
WTXIDRELAY before its VERACK; any other peer is left before anything is announced. Every other
message is read and ignored; the tool never answers it. A peer that declines the transaction,
never asks, or never answers the PING sees nothing different from a peer that does everything
promptly, except that the connection ends at a fixed deadline instead of earlier.

## The schedule, in plain words

Times are from job start.

- **Discovery, 0-18 s.** The tool asks Tor to resolve each release DNS seed four times, each
  query on its own Tor stream, and keeps up to three answers per seed; it adds up to eight
  onion peers from the release fixed-seed list. Queries are cut off at 15 s, and delivery
  starts at 18 s regardless. Slow or missing answers delay nothing.
- **Delivery: six slots.** A slot is one delivery target with a first peer and up to three
  pre-chosen backups. Three prompt slots open together at 18 s, two to peers reached through
  Tor exits and one to an onion peer, racing three paths for the first announcement. The
  other three open at times drawn at job start: one onion slot 35-180 s after delivery starts
  (53-198 s), and two exit-path slots 185-240 s after it (203-258 s), at least 5 s apart. A
  slot's class is a preference: an onion slot whose onion list has run out falls back to
  exit-path peers, so with few onions known every connection can be exit-path; exit-path slots
  never take onions.
- **Each connection** has 45 s from its scheduled time to connect, finish the encrypted
  handshake and announce; then 75 s for the peer to ask for the transaction; then 10 s for the
  PONG after the PING is written. If it fails before announcing, including a peer that closes
  the encrypted handshake because it speaks only the old transport (there is no v1 fallback),
  the slot tries its next pre-chosen peer 50-60 s (drawn at job start) after the previous
  scheduled time, at most three times. Once a slot has announced it is done, whatever happens
  next. No slot ever waits for another.
- **End.** Every slot's network work ends within 310 s of its first scheduled time, so all of
  it is over by 568 s at the latest (a hard cap of 10 min backs this up), and usually much
  earlier. These are scheduled bounds: host scheduling can delay a step, never reschedule one.

All durations are compile-time constants (`src/privbcast/*.h`). There are no knobs: a
tunable would make its users distinguishable.

Where the numbers come from: the 50 s backup floor, the 310 s slot and the 568 s bound are
derived from the per-connection budgets; the 18 s discovery window and the 30 s parent hold are
measured (RESOLVE bursts finished within 8 s nine times in ten on one Tor client; orphan
resolution asks within about 4 s on signet); the onion reachability under Limits comes from
probing each release's fixed-seed list. Six slots, three prompt, three backups each, and the two
windows are choices: enough that losing a path or a few peers does not lose the job, few enough
that a job stays a small event. None is a privacy parameter; changing one changes cost and
robustness for every user of a release alike.

The schedule makes no claim about hiding the user's address: that comes entirely from the
tool being nodeless and reaching everything through Tor. What the schedule buys is delivery
that survives failed connections, a peer that can move nothing, and only marginal, unpromised
privacy on top: the later slots open at random offsets within fixed windows, in a fixed order
and at least 5 s apart, and there is no regular grid to recognise. The prompt trio is
deliberately simultaneous, and backups can still cluster; bursts are not eliminated.

## One parent, one child

A transaction whose fee is too low to enter mempools on its own can be carried by a child
that spends it, when the recipient evaluates the two together (Bitcoin Core 28 and later do
this for exactly one parent and one child). Give the tool both transactions, in either order,
and it works out which is which.

- Only the child is announced. Announcing the parent would invite a request for it before
  the child; a low-fee parent received alone is rejected, and the tool would then have to
  serve it a second time, which it never does.
- The child is served once, on the exact single-entry request for it by wtxid. The tool then
  holds for 30 s for the peer to ask for the parent, which a recipient that lacks it does about
  4 s later (its orphan-resolution delays; measured on signet). That request may batch the parent
  with the child's other inputs, so the tool serves the parent and, like any node, answers the
  entries it does not have with `notfound`; it never says that about its own two transactions.
- The parent is served once, only if it is the parent given, and only on one of two requests:
  - after the child was served, a request naming the parent by txid (orphan resolution);
  - before the child was served, a request naming the parent and not the child, from a recipient
    that already held the child as an orphan learned from another peer (our wtxid announcement
    adds us as an announcer of that orphan, so it asks us only for the parent).
  A request that names the child alongside the parent before the child was served is ignored
  outright, with no `notfound` either: a peer we have not served cannot have learned the child's
  inputs from us, so that request is not one our announcement made possible. The connection stays
  open and the child's own request is still answered afterwards. Once the parent is served, PING
  goes out and nothing more is served on that connection.
- If no request for the parent arrives within the hold, PING goes out anyway; the tool cannot
  tell whether the peer already had the parent, was still waiting on a request to another peer,
  or will not take the package.
- One request window still bounds the whole exchange; the second request restarts nothing,
  and the announcement point and the replacement rules are unchanged.
- Which recipients accept the package depends on the parent. A parent below the minimum relay
  feerate is accepted as part of a package by Bitcoin Core 28 and later only if it is TRUC
  (version 3); a non-TRUC parent below that feerate needs Bitcoin Core 31 or later, and older
  recipients drop it. Whether the package reaches miners depends on what they run.
- The tool cannot check that the child has no other unconfirmed parent, or that the child pays
  enough for both, and there is no dry run for a package: `testmempoolaccept` checks each
  transaction on its own and does not apply the child's fee to the parent, so it reports a
  low-fee parent as "min relay fee not met" even when the package would be accepted, and
  stops there without evaluating the child at all, so that rejection says nothing about the
  child's validity. Work out the package feerate yourself. Nor can the tool see whether the
  recipient accepted the package; a PONG means only that the recipient processed what we sent.
  A recipient older than Core 28 asks for the parent, rejects it alone and keeps the child as an
  orphan only until we disconnect.
- Serving a second transaction on request happens only in package mode (this tool given two
  transactions, or `submitpackage` under `-privatebroadcast`), so a recipient that asks for the
  parent learns the sender used package mode. That the two transactions belong together is
  already visible on the chain.

## Using it

1. Check the transaction with `bitcoin-cli testmempoolaccept` first. The tool does only
   stateless sanity checks itself. The check leaves the node's validation and coins caches
   untouched, but reading the inputs warms the node's database and page caches like any
   UTXO lookup; if even that matters to you, run the check on a node that is not your
   public one. For a parent and child it rejects a low-fee parent on its own; see "One
   parent, one child".
2. Feed the final hex on stdin: `bitcoin-privbcast send < tx.hex` (Tor at 127.0.0.1:9050;
   `-tor=` for another listener). The report, as JSON, goes to stdout; progress lines to
   stderr.
3. Watch for receipt with `bitcoin-cli getmempoolentry` if you want to know it worked.
   Do not also broadcast the same transaction the ordinary way.

`send` exits 0 if at least one announcement was fully written, 2 if none was, and 1 on bad
input or arguments. The JSON report has no wall-clock value and is the record; progress lines
on stderr are best effort (a line a pipe cannot take is dropped, never waited for; see `-help`).
Both are local evidence that a broadcast happened: keep them where you would keep a wallet log.

The proxy must be on this machine (a remote one would carry destinations and credentials in the
clear), and it must accept SOCKS authentication, the RESOLVE extension and IPv6. Nothing checks
that it is Tor, because a proxy that is not Tor delivers nothing: every stream requires fresh
username/password credentials, which `ssh -D` and most VPN clients do not offer, exit-path
candidates come only from RESOLVE answers, and the bundled list is onion only. Such a proxy
learns that a job ran, not which transaction it carried.

The fresh credentials isolate streams only while the SocksPort keeps `IsolateSOCKSAuth`, which
is Tor's default. With `NoIsolateSOCKSAuth`, Tor may put several of a job's streams on one
circuit, so one exit can see the transaction reach several recipients, and it may put a job's
stream on a circuit that also carries other traffic through the same SocksPort, such as a
node's own connections and whatever they reveal about it (its advertised onion address, for
one). Nothing at the SOCKS interface can detect this; Tor accepts the credentials either way.

## Inside the node

With `-privatebroadcast`, `sendrawtransaction` and `submitpackage` queue a job that runs this
code in the node's process. The job still uses none of the node's peer machinery: no address
manager, connection manager, peer manager or ban list. Its discovery, schedule and wire profile
are the tool's. The transaction does not enter the node's mempool until it comes back from the
network, and the node then treats it like any other. Submitting the same transaction again
queues another job, including one the node's mempool already holds; jobs are not deduplicated.

Node settings that choose peers (`-onlynet`, `-dnsseed`, `-fixedseeds`, `-connect`, `-seednode`,
`-addnode`) do not apply to jobs. A node with `-onlynet=onion` still gets exit-path connections:
the fixed-seed onions alone age with the release (see Limits), Tor hides the user's address on
either path, and an exit can only drop or alter its own connection, which no job depends on.
There is no switch, for the same reason there are no other knobs.

What a job does share:

- **The proxy.** The node's Tor proxy, trusted as the node's other proxy settings are. Every
  stream carries fresh credentials whatever `-proxyrandomize` says.
- **The queue.** At most two jobs run at once, in submission order. A job ends when its last
  connection ends, so a recipient that holds a connection open can delay when the next queued
  job starts. That is a link between two of this node's transactions, not a node identifier.
- **An observation.** When the node's mempool first sees the transaction, which is recorded in
  the report and never fed back into a job.
- **The log.** `debug.log` records job progress only with `-debug=privatebroadcast`. SOCKS
  failures mentioning a destination only appear with `-debug=proxy` or `-debug=net`.
- **The process.** Shutdown and `setnetworkactive false` cancel jobs.

Wallet sends are not private broadcasts: the wallet submits to the node's mempool and
rebroadcasts from there, which a private broadcast must not do. Making them private is a
separate change.

## Limits

- The tool and the node still share a host and, usually, a Tor daemon. Load, Tor's own
  caches and the client's guard are common to both; the design does not separate them.
- DNS seeds, resolvers and Tor exits are untrusted. They can lie, tag their answers, or
  act as the peer themselves on an exit-path connection, since the encrypted transport
  authenticates nobody. The onion slot has no exit in the path.
- Onion peers come only from the fixed-seed list shipped with the release, which ages with it:
  a year after a list is generated, roughly one onion in six still accepts the encrypted
  transport. On an old release most onion slots fall back to exit-path peers, and the job loses
  its paths without an exit.
- Recipients can recognise the release by its profile and probe it. They learn that
  someone used the tool, not who.
- Discovery does not exclude the node's own addresses; a filter on node state is the coupling
  rule 1 forbids. A job that draws the node's own onion or IP makes the node one of that job's
  first relayers rather than none of them.
- Tor's own timing and reachability vary between users; the schedule fixes when the tool
  acts, not how fast the network answers.
- A job stops when its schedule ends and never retries on its own: retrying because the
  transaction did not come back would act on exactly the signal a network adversary controls.
  To censor a job, an adversary must control or silence every slot's recipients and backups,
  onion slots included. The remedy is another job, submitted by the user after watching
  `getmempoolentry`; it discovers and schedules independently.
