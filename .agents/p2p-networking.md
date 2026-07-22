# P2P and networking

This guide covers durable rules for contributing to Bitcoin Core's P2P networking code (`src/net*`, `src/protocol*`, and related test infrastructure).

## Protocol design process

- **Discuss publicly first** — Before implementing complicated or controversial P2P protocol changes, discuss them on the bitcoin-dev mailing list or [Delving Bitcoin](https://delvingbitcoin.org/). Consensus changes have the additional BIP and broad-consensus requirements in [the consensus guide](consensus-validation.md).
- **Link to design docs** — When introducing BIP-defined features, reference the BIP PR or Delving post in commit messages and release notes.

## Architecture & encapsulation

- **Transport abstraction** — Encapsulate V1/V2 transport differences within transport classes; do not expose transport type to higher-level network code.
- **Directional control flow** — `PeerManager` tells `CConnman` what to do (e.g., `SetTryNewOutboundPeer`). Do not misuse `NetEventsInterface` for getters or non-event functionality.
- **Option routing** — Route net_processing options through `PeerManager::Options` and `ApplyArgsManOptions()`. Do not reference `gArgs` directly in `net_processing.cpp`.

## Code correctness

- **No assertions on peer input** — Never use `assert()` to validate data received from peers. Use `Assume()` for internal invariants only (e.g., in addrman for corruption detection); failures there should not crash the node.
- **Disconnect for protocol violations** — When implementing new protocol features, disconnect peers for well-defined invalid behavior. Do not silently ignore violations.
- **Mutex discipline** — Do not hold `m_peer_mutex` longer than necessary. Take a snapshot of peer shared pointers, then release the mutex early.

## Testing

- **No sleeps or log scanning** — Synchronize tests through observable peer/node state (`getpeerinfo()`, `sync_send_with_ping`, etc.).
- **Use `sync_send_with_ping`** — When checking that messages have been sent, use `sync_send_with_ping` (waits for send queue flush), not `sync_with_ping`.
- **Wait for disconnection** — After disconnecting a peer, confirm `CNode` destruction via `getpeerinfo()` rather than timing-based waits.
- **Verify connection properties** — Confirm protocol version, session IDs, and transport type from the node side using `getpeerinfo()` to guarantee both sides agree.
- **V2 transport thread safety** — `ChaCha20` is stateful; sending encrypted messages from multiple threads concurrently without synchronization causes connection breakage.

Refer to [the P2P section of the project documentation](../doc/p2p.md) and [`CONTRIBUTING.md`](../CONTRIBUTING.md) for canonical process details.
