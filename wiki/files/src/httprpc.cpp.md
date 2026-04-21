---
kind: file
title: src/httprpc.cpp
status: active
last_reviewed: 2026-04-21
paths:
  - src/httprpc.cpp
tags:
  - rpc
  - http
  - auth
---

# src/httprpc.cpp

## Role in the System

`src/httprpc.cpp` is the HTTP JSON-RPC ingress layer. It authenticates incoming
RPC requests, enforces the main HTTP method/path rules, converts HTTP payloads
into `JSONRPCRequest` objects, dispatches through `JSONRPCExec()`, and
registers the `/` and `/wallet/` JSON-RPC HTTP handlers.

## Important Types and Functions

- `RPCAuthorized()` performs HTTP auth processing and returns the authenticated
  username when successful.
- `ExecuteHTTPRPC()` bridges parsed JSON into the JSON-RPC execution path.
- `HTTPReq_JSONRPC()` is the main request handler for JSON-RPC HTTP traffic.
- `StartHTTPRPC()` registers the HTTP handlers.
- `StopHTTPRPC()` unregisters them during shutdown.

## Callers and Dependencies

- Started from `AppInitServers()` in `src/init.cpp`.
- Depends on `src/rpc/server.cpp` (`JSONRPCExec`), the HTTP server layer, and
  wallet routing conventions that use the `/wallet/<walletname>/` URI prefix.
- Works with wallet RPC utilities and docs that define how multiwallet routing
  should behave.

## Related Tests

- `test/functional/interface_http.py`
- `test/functional/interface_rpc.py`
- `test/functional/interface_bitcoin_cli.py`
- `test/functional/rpc_users.py`
- `test/functional/rpc_whitelist.py`

## Notes or Risks

- Critical categories:
  - `offline`: regressions here can make the node's RPC surface unavailable
    even when the underlying node remains healthy.
  - `operator privacy`: auth handling, wallet URI routing, and request-path
    behavior can leak wallet names or other metadata if mishandled.
  - `resource`: malformed or abusive HTTP traffic reaches this layer before RPC
    method dispatch.
- The `/wallet/` registration boundary matters because wallet name selection is
  partly encoded in the request URI, not only in RPC params.
- Because this file is the ingress path before `JSONRPCExec()`, it is the
  first review surface for authentication bypass, path confusion, or
  availability regressions in RPC serving.

## Sources Consulted

- `src/httprpc.cpp`
- `src/init.cpp`
- `src/rpc/server.cpp`
- `src/wallet/rpc/util.cpp`
- `doc/JSON-RPC-interface.md`
- `test/functional/interface_http.py`
- `test/functional/interface_rpc.py`
- `test/functional/interface_bitcoin_cli.py`
- `test/functional/rpc_users.py`
- `test/functional/rpc_whitelist.py`
