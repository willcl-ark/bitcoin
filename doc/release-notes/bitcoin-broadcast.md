New standalone utility
----------------------

`bitcoin-broadcast` hands one raw transaction from stdin to an explicitly selected
Tor v3 onion Bitcoin peer through an authenticated local SOCKS proxy. It runs
without node, wallet, RPC, configuration, or data-directory state. Tor must be
configured with `IsolateSOCKSAuth`.

The tool prefers v2 transport with one eligible v1 fallback. It stops trying peers
after possible transaction disclosure. Silent status 0 confirms a complete
transaction write followed by a matching ping/pong, not mempool acceptance or
propagation. See `doc/bitcoin-broadcast.md` for usage and privacy limits. Build with
`-DBUILD_BROADCAST=ON`; the default follows `BUILD_TESTS`.
