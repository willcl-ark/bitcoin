# bitcoin-privbcast: design notes

The goal is one thing: broadcast a transaction without ever revealing the sender's permanent
identity, its IP address or its onion address. Everything else here serves that.

`bitcoin-privbcast` announces one final transaction to a small, bounded set of peers over Tor,
and then stops: at most 24 connections, all over within ten minutes, nothing kept afterwards.
It is a separate program from `bitcoind`, and that separation is the design. Two rules make it.
The tool touches no node state, so nothing a recipient sees at the P2P layer can be tied to a
node. Its schedule is drawn before the first connection and never moved by anything a peer
does, so nothing the network does can steer it. The tool does not promise delivery, does not
hide that a job ran, and does not retry on its own.

## Who sees what

Every party sees a Tor circuit and never the sender.

- **A recipient**: an exit or an onion circuit, a constant wire profile, and the transaction.
  No IP, onion address, peer set, address manager, mempool or validation cache exists on the
  job's side to leak.
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
the tool; the user agent is a constant that no node sends (see bitcoin/bitcoin#27509). It
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
derived from the per-connection budgets; the 18 s discovery window is measured (RESOLVE bursts
finished within 8 s nine times in ten on one Tor client); the onion reachability under Limits
comes from probing each release's fixed-seed list. Six slots, three prompt, three backups each,
and the two windows are choices: enough that losing a path or a few peers does not lose the job,
few enough that a job stays a small event. None is a privacy parameter; changing one changes cost
and robustness for every user of a release alike.

The schedule makes no claim about hiding the user's address: that comes entirely from the
tool being nodeless and reaching everything through Tor. What the schedule buys is delivery
that survives failed connections, a peer that can move nothing, and only marginal, unpromised
privacy on top: the later slots open at random offsets within fixed windows, in a fixed order
and at least 5 s apart, and there is no regular grid to recognise. The prompt trio is
deliberately simultaneous, and backups can still cluster; bursts are not eliminated.

## Using it

1. Check the transaction with `bitcoin-cli testmempoolaccept` first. The tool does only
   stateless sanity checks itself. The check leaves the node's validation and coins caches
   untouched, but reading the inputs warms the node's database and page caches like any
   UTXO lookup; if even that matters to you, run the check on a node that is not your
   public one.
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
- Tor's own timing and reachability vary between users; the schedule fixes when the tool
  acts, not how fast the network answers.
