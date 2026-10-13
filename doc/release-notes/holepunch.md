P2P and network changes
-----------------------

- Experimental TCP hole punching is available with `-holepunch`. The optional
  `-holepunchcoordinator` and `-holepunchonion` roles enable coordination through
  ordinary peers or dedicated Tor services. All roles require v2 transport.
  `-punchport` and `-punchbind` configure the local punch sockets. The default
  punch port is chosen randomly from 49152 to 65535, excluding bad ports, and
  remains fixed until the process exits. IPv4 and IPv6 use separate coordination
  onion identities, replaced whenever their external endpoints change. See
  [doc/holepunch.md](../holepunch.md) and the
  [draft BIP](https://github.com/0xB10C/bips/blob/17bbcc637049b9ce63a0faac91e0a9330481193f/bip-tcp-holepunch.md).
- `getpeerinfo` reports `outbound-holepunch` and `holepunch-coordination`
  connection types.
