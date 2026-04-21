---
kind: area
title: Libbitcoinkernel and Libraries
status: active
last_reviewed: 2026-04-20
paths:
  - src/kernel/
  - src/secp256k1/
  - src/leveldb/
  - src/minisketch/
  - src/univalue/
  - src/crc32c/
tags:
  - libbitcoinkernel
  - libraries
  - vendored
---

# Libbitcoinkernel and Libraries

## Summary

This tree contains one in-progress library boundary for external consumers,
`src/kernel/`, and several bundled libraries that Bitcoin Core links into its
own targets.

- `src/kernel/` builds the experimental `bitcoinkernel` library.
- `src/secp256k1/` provides elliptic-curve cryptography primitives used across signing, verification, Taproot, EllSwift, and MuSig code.
- `src/leveldb/` and `src/crc32c/` provide the storage engine and checksum code used by Bitcoin Core's LevelDB-backed databases.
- `src/univalue/` provides the JSON/value representation used by RPC, REST, CLI, wallet, and IPC code.
- `src/minisketch/` provides BCH-based set reconciliation primitives used by node transaction reconciliation code and by tests/fuzzers.

# Responsibilities and invariants

- `src/kernel/bitcoinkernel.h` documents `bitcoinkernel` as an unversioned, unstable C API that is not yet included in Bitcoin Core releases. It currently exposes block validation, script verification, block index traversal, and block/undo reading interfaces.
- `src/kernel/CMakeLists.txt` confirms that `bitcoinkernel` is not yet a narrow consensus-only library. The target still pulls in `validation.cpp`, `node/blockstorage.cpp`, `node/chainstate.cpp`, `txdb.cpp`, policy code, LevelDB objects, CRC32C objects, and `secp256k1_objs`. The file's TODO comment says the library is still being decoupled.
- `src/leveldb/` is wrapped by `CDBWrapper` (`src/dbwrapper.h`, `src/dbwrapper.cpp`). Current Bitcoin Core callers include `CCoinsViewDB` in `src/txdb.h` and `BlockTreeDB` in `src/node/blockstorage.h`, so the bundled LevelDB copy is part of chainstate and block index persistence.
- `src/crc32c/` is consumed by the bundled LevelDB implementation rather than by high-level Bitcoin-specific code. `src/leveldb/port/port_stdcxx.h`, `src/leveldb/db/log_writer.cc`, `src/leveldb/db/log_reader.cc`, and `src/leveldb/table/table_builder.cc` call the CRC32C routines directly.
- `src/univalue/` builds a static library in `src/univalue/CMakeLists.txt`. Current in-tree users include `src/httprpc.cpp`, `src/rest.cpp`, `src/bitcoin-tx.cpp`, `src/external_signer.cpp`, and wallet/RPC test code.
- `src/secp256k1/` builds both `secp256k1` and `secp256k1_objs` (`src/secp256k1/src/CMakeLists.txt`). The latter exists so parent projects can link secp256k1 object files into static libraries such as `bitcoinkernel`.
- `src/minisketch/` builds the `minisketch` library (`src/minisketch/src/CMakeLists.txt`). Bitcoin Core wraps it in `src/node/minisketchwrapper.cpp`, and `src/CMakeLists.txt` links it into `bitcoin_node`, `test_bitcoin`, and `fuzz`.

## Important code paths

- `src/kernel/CMakeLists.txt`
  Defines the `bitcoinkernel` target, the `libbitcoinkernel` convenience target, and install rules for the header and pkg-config metadata.
- `src/kernel/bitcoinkernel.h`
  Defines the external C ABI and its current scope.
- `src/kernel/bitcoinkernel.cpp`
  Implements the exported API and owns the library's glue to logging, validation notifications, block/undo I/O, and script verification.
- `src/dbwrapper.h` and `src/dbwrapper.cpp`
  Define Bitcoin Core's LevelDB adapter layer.
- `src/txdb.h`
  Shows LevelDB-backed UTXO storage through `CCoinsViewDB`.
- `src/node/blockstorage.h`
  Shows LevelDB-backed block index storage through `BlockTreeDB`.
- `src/key.cpp`, `src/pubkey.cpp`, `src/musig.cpp`
  Show the main secp256k1 call sites in the current tree.
- `src/node/minisketchwrapper.cpp`
  Chooses a minisketch implementation at runtime by benchmarking the available variants once.

## Related tests

- Kernel library coverage: `src/test/kernel/test_kernel.cpp`.
- LevelDB wrapper coverage: `src/test/dbwrapper_tests.cpp`.
- Minisketch wrapper coverage: `src/test/minisketch_tests.cpp`, `src/test/fuzz/minisketch.cpp`.
- UniValue subtree tests: `src/univalue/CMakeLists.txt` (`unitester`, `object`).
- Bundled library native test/bench support: `src/secp256k1/src/CMakeLists.txt`, `src/minisketch/src/CMakeLists.txt`, `src/leveldb/CMakeLists.txt`, `src/crc32c/CMakeLists.txt`.

## Adjacent pages

- `[[areas/consensus-and-script]]`
- `[[areas/validation-and-chainstate]]`
- `[[areas/build-packaging-and-ci]]`

## Sources consulted

- `src/CMakeLists.txt`
- `src/kernel/CMakeLists.txt`
- `src/kernel/bitcoinkernel.h`
- `src/kernel/bitcoinkernel.cpp`
- `src/interfaces/README.md`
- `src/secp256k1/README.md`
- `src/secp256k1/src/CMakeLists.txt`
- `src/minisketch/README.md`
- `src/minisketch/src/CMakeLists.txt`
- `src/leveldb/README.md`
- `src/leveldb/CMakeLists.txt`
- `src/univalue/CMakeLists.txt`
- `src/crc32c/README.md`
- `src/dbwrapper.h`
- `src/dbwrapper.cpp`
- `src/txdb.h`
- `src/node/blockstorage.h`
- `src/node/minisketchwrapper.cpp`
- `src/key.cpp`
- `src/pubkey.cpp`
- `src/musig.cpp`
