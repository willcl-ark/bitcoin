P2P and network changes
-----------------------

- Experimental TCP hole punching is available with `-holepunch`. The optional
  `-holepunchcoordinator` and `-holepunchonion` roles enable coordination through
  ordinary peers or a dedicated Tor service. All roles require v2 transport.
  `-punchport` and `-punchbind` configure the local punch sockets. See
  [doc/holepunch.md](../holepunch.md) and the
  [draft BIP](https://github.com/willcl-ark/bips/blob/a9860ba2ab7cb048209bf69f1e313a31200e1aa2/bip-tcp-holepunch.md).
- `getpeerinfo` reports `outbound-holepunch` and `holepunch-coordination`
  connection types.
