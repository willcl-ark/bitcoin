P2P and network changes
-----------------------

- `-externalip` addresses are now considered manual local addresses and are
  advertised regardless of `-onlynet`, matching the documented behaviour of
  `onlynet`. This affects local Tor (`-listenonion=1`) and I2P (`-i2psam=...
  -i2pacceptincoming=1`) listening addresses, which are now advertised even with
  onlynet set, e.g. `-onlynet=ipv4`. (#34538)
