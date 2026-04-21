---
kind: area
title: RPC, REST, ZMQ, and Interfaces
status: active
last_reviewed: 2026-04-20
paths:
  - src/rpc/
  - src/rest.cpp
  - src/httprpc.cpp
  - src/interfaces/
  - src/zmq/
tags:
  - rpc
  - rest
  - zmq
  - interfaces
---

# RPC, REST, ZMQ, and Interfaces

## Summary

This area covers the main external control and notification surfaces for Bitcoin Core, plus the internal abstraction boundaries used to connect node, wallet, GUI, CLI, and multiprocess code. The current tree separates transport (`src/httprpc.cpp`, `src/rest.cpp`, `src/zmq/`) from method dispatch (`src/rpc/`) and from cross-component interfaces (`src/interfaces/`).

## Responsibilities and Invariants

- Server startup is coordinated in `src/init.cpp` (`AppInitServers`): initialize the HTTP server, mark RPC running with `StartRPC()`, register JSON-RPC HTTP handlers with `StartHTTPRPC()`, optionally enable REST with `StartREST()`, then start accepting HTTP traffic with `StartHTTPServer()`.
- The JSON-RPC HTTP entry point is `HTTPReq_JSONRPC()` in `src/httprpc.cpp`. It only accepts POST, authenticates first, parses the request body, and forwards to `ExecuteHTTPRPC()`.
- `ExecuteHTTPRPC()` builds a `JSONRPCRequest` and forwards execution to `JSONRPCExec()`, which dispatches through the global `tableRPC` and `CRPCTable::execute()` in `src/rpc/server.cpp`.
- `StartHTTPRPC()` registers `/` unconditionally and `/wallet/` only when `g_wallet_init_interface.HasWalletSupport()` is true (`src/httprpc.cpp`). The local interface doc in `doc/JSON-RPC-interface.md` documents `/wallet/<walletname>/` as the required endpoint for wallet RPCs when multiple wallets are loaded.
- REST is a separate HTTP surface, not a wrapper around `tableRPC`. `StartREST()` in `src/rest.cpp` registers fixed `/rest/...` handlers such as `/rest/tx/`, `/rest/block/`, `/rest/headers/`, `/rest/getutxos`, and `/rest/chaininfo`. `AppInitServers()` only enables it when `-rest` is true.
- `src/interfaces/` defines C++ abstraction boundaries between major components. `interfaces::Init` in `src/interfaces/init.h` can vend `Node`, `Chain`, `Mining`, `WalletLoader`, `Rpc`, and `Ipc` implementations depending on the process (`bitcoind`, `bitcoin-node`, `bitcoin-gui`, `bitcoin-qt`, `bitcoin-wallet`).
- `interfaces::Rpc` in `src/interfaces/rpc.h` is an in-process adapter for HTTP-style RPC execution. `node::RpcImpl::executeRpc()` in `src/node/interfaces.cpp` fills a `JSONRPCRequest` and calls `ExecuteHTTPRPC()`.
- ZMQ is an outbound notification system, not a request interface. `CZMQNotificationInterface::Create()` in `src/zmq/zmqnotificationinterface.cpp` instantiates notifiers from configured `-zmqpub*` options, and `src/init.cpp` registers the resulting object as a validation interface when present.
- ZMQ notifications are emitted from validation callbacks in `src/zmq/zmqnotificationinterface.cpp`: `UpdatedBlockTip`, `TransactionAddedToMempool`, `TransactionRemovedFromMempool`, `BlockConnected`, and `BlockDisconnected`. `BlockConnected()` explicitly skips historical assumeutxo background-validation blocks when `role.historical` is true.

## Important Code Paths

- Startup and option gates: `src/init.cpp` (`AppInitServers`, ZMQ option registration, ZMQ initialization/registration)
- JSON-RPC transport: `src/httprpc.cpp` (`HTTPReq_JSONRPC`, `ExecuteHTTPRPC`, `StartHTTPRPC`, `StopHTTPRPC`)
- RPC dispatch core: `src/rpc/server.cpp` (`CRPCTable`, `JSONRPCExec`, `StartRPC`, `InterruptRPC`, `StopRPC`)
- REST handlers: `src/rest.cpp` (`uri_prefixes`, `StartREST`, `StopREST`)
- Internal interfaces: `src/interfaces/README.md`, `src/interfaces/init.h`, `src/interfaces/node.h`, `src/interfaces/rpc.h`, `src/interfaces/wallet.h`
- In-process RPC bridge: `src/node/interfaces.cpp` (`node::RpcImpl::executeRpc`, `interfaces::MakeRpc`)
- ZMQ notification and RPC exposure: `src/zmq/zmqnotificationinterface.cpp`, `src/zmq/zmqpublishnotifier.cpp`, `src/zmq/zmqrpc.cpp`

## Compile and Runtime Gates

- JSON-RPC serving is started from `AppInitServers()`; CLI and GUI availability is still gated by process startup choices and `-server` behavior documented in `doc/JSON-RPC-interface.md`
- REST runtime gate: `-rest` in `src/init.cpp`
- IPC runtime gate: `-ipcbind=<address>` in `src/init.cpp` for processes whose `interfaces::Init::canListenIpc()` returns true
- ZMQ compile gate: `ENABLE_ZMQ` in `src/init.cpp` and `src/zmq/CMakeLists.txt`
- ZMQ runtime gates: `-zmqpubhashblock`, `-zmqpubhashtx`, `-zmqpubrawblock`, `-zmqpubrawtx`, `-zmqpubsequence`, plus per-topic `...hwm` options in `src/init.cpp`

## Related Tests

- Unit tests: `src/test/rpc_tests.cpp`, `src/test/rest_tests.cpp`, `src/test/interfaces_tests.cpp`
- Functional coverage: `test/functional/interface_rpc.py`, `test/functional/interface_http.py`, `test/functional/interface_rest.py`, `test/functional/interface_zmq.py`, `test/functional/interface_ipc.py`, `test/functional/interface_bitcoin_cli.py`, `test/functional/rpc_bind.py`, `test/functional/rpc_users.py`, `test/functional/rpc_whitelist.py`

## Adjacent Pages

- `[[areas/wallet]]`
- `[[areas/p2p-and-networking]]`
- `[[areas/common-utils-and-configuration]]`
- `[[concepts/operator-privacy]]`
- `[[workflows/rpc-request-handling]]`
- `[[files/src/httprpc.cpp]]`
- `[[investigations/critical-codepaths-priority-map]]`

## Sources Consulted

- `src/init.cpp`
- `src/httprpc.cpp`
- `src/rest.cpp`
- `src/rpc/server.cpp`
- `src/node/interfaces.cpp`
- `src/interfaces/README.md`
- `src/interfaces/init.h`
- `src/interfaces/node.h`
- `src/interfaces/rpc.h`
- `src/interfaces/wallet.h`
- `src/zmq/zmqnotificationinterface.cpp`
- `src/zmq/zmqrpc.cpp`
- `doc/JSON-RPC-interface.md`
- `doc/zmq.md`
