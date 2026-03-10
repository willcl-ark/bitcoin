Packaging change
================

The CMake install tree is now the source of truth for auxiliary package files.
As a result, package consumers should expect the example configuration,
README, and `rpcauth` helper files under the Bitcoin-specific data directory:

- `share/bitcoin/bitcoin.conf`
- `share/bitcoin/README.md` on Linux and macOS
- `share/bitcoin/README_windows.txt` on Windows
- `share/bitcoin/rpcauth/`

This replaces the previous Guix archive layout, which placed `README.md` and
`bitcoin.conf` at the archive root and `rpcauth/` under `share/`.

Any downstream packaging, archive inspection, or release automation that
expects the old paths must be updated to read the installed files from
`share/bitcoin/` instead.
