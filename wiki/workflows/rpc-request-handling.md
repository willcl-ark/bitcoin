---
kind: workflow
title: RPC Request Handling
status: active
last_reviewed: 2026-04-21
paths:
  - src/init.cpp
  - src/httprpc.cpp
  - src/rpc/server.cpp
  - src/rest.cpp
  - src/wallet/rpc/util.cpp
tags:
  - rpc
  - http
  - wallet
---

# RPC Request Handling

## Summary

Bitcoin Core's main RPC workflow is: start the HTTP/RPC surfaces, register
method handlers, receive an authenticated HTTP request, decode it into a
`JSONRPCRequest`, dispatch through `tableRPC`, and optionally route wallet
methods based on the `/wallet/<walletname>/` URI prefix. REST uses the same
HTTP server but is registered separately and does not dispatch through
`tableRPC`.

## Workflow

### 1. Server startup and registration

1. `src/init.cpp` (`AppInitServers`) starts the HTTP server machinery,
   activates RPC with `StartRPC()`, registers JSON-RPC handlers with
   `StartHTTPRPC()`, optionally enables REST with `StartREST()`, and then
   starts serving requests.
2. `src/httprpc.cpp` (`StartHTTPRPC`) registers the root JSON-RPC handler at
   `/`, and also registers `/wallet/` when wallet support is available.
3. `src/rest.cpp` (`StartREST`) registers fixed `/rest/...` endpoints directly
   with the HTTP server.
4. Wallet RPC commands are appended into the global RPC table through
   `src/wallet/interfaces.cpp` (`WalletLoaderImpl::registerRpcs`) using
   `GetWalletRPCCommands()`.

### 2. HTTP JSON-RPC request entry

1. `src/httprpc.cpp` (`HTTPReq_JSONRPC`) receives the HTTP request.
2. It enforces POST, authenticates, parses the request body, and calls
   `ExecuteHTTPRPC(...)`.
3. `ExecuteHTTPRPC(...)` builds a `JSONRPCRequest` and forwards it to
   `JSONRPCExec(...)`.

### 3. Table dispatch

1. `src/rpc/server.cpp` (`JSONRPCExec`) dispatches through the global
   `CRPCTable tableRPC`.
2. `CRPCTable::execute(...)` resolves the named `CRPCCommand` and invokes its
   actor with the filled request context.
3. The resulting `UniValue` response is serialized back through the HTTP
   layer.

### 4. Wallet routing

- Wallet selection comes from the request URI, not just the RPC method name.
- `src/wallet/rpc/util.cpp` (`GetWalletNameFromJSONRPCRequest`) extracts the
  wallet name from `/wallet/<walletname>/`.
- If multiple wallets are loaded and no wallet endpoint is specified, wallet
  RPC utilities return an error instructing the caller to use the wallet URI.

### 5. REST boundary

- `src/rest.cpp` handlers are separate HTTP handlers.
- They do not go through `tableRPC` or `JSONRPCExec()`, even though they share
  the same underlying HTTP server.

## Related Tests

- `test/functional/interface_rpc.py`
- `test/functional/interface_http.py`
- `test/functional/interface_rest.py`
- `test/functional/interface_bitcoin_cli.py`

## Adjacent Pages

- `[[areas/rpc-rest-zmq-and-interfaces]]`
- `[[areas/wallet]]`

## Sources Consulted

- `src/init.cpp`
- `src/httprpc.cpp`
- `src/rpc/server.cpp`
- `src/rest.cpp`
- `src/wallet/interfaces.cpp`
- `src/wallet/rpc/util.cpp`
- `doc/JSON-RPC-interface.md`
- `test/functional/interface_rpc.py`
- `test/functional/interface_http.py`
- `test/functional/interface_rest.py`
