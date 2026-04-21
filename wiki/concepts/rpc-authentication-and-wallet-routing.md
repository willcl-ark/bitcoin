---
kind: concept
title: RPC Authentication and Wallet Routing
status: active
last_reviewed: 2026-04-21
paths:
  - src/httpserver.h
  - src/httpserver.cpp
  - src/httprpc.cpp
  - src/rpc/server.h
  - src/rpc/server.cpp
  - src/wallet/rpc/util.cpp
tags:
  - rpc
  - http
  - auth
  - wallet
---

# RPC Authentication and Wallet Routing

## Summary

Bitcoin Core's JSON-RPC boundary is layered. `src/httpserver.cpp` decides which
peers can reach which registered HTTP paths and can shed load before a request
reaches JSON-RPC. `src/httprpc.cpp` then enforces POST plus HTTP Basic auth,
parses the body into a `JSONRPCRequest`, and applies per-user method
whitelists. `src/rpc/server.cpp` dispatches by method name after a global
warmup check. Wallet targeting happens later in `src/wallet/rpc/util.cpp` by
interpreting `request.URI`; it selects a wallet for wallet-aware RPC handlers,
but it is not a second authentication layer.

## Responsibilities and Invariants

- `src/httpserver.cpp` (`InitHTTPAllowList`, `HTTPBindAddresses`) is the first
  network gate. Loopback is always allowed by default; `-rpcallowip` expands
  the allowlist, and `-rpcbind` is ignored unless `-rpcallowip` is also set.
- `src/httprpc.cpp` (`StartHTTPRPC`) registers only two JSON-RPC HTTP entry
  paths: exact `/`, and `/wallet/` as a prefix when
  `g_wallet_init_interface.HasWalletSupport()` is true. Other paths are
  rejected by `src/httpserver.cpp` (`http_request_cb`) before JSON-RPC auth or
  dispatch.
- `src/httpserver.cpp` (`http_request_cb`) rejects unknown HTTP methods,
  requests from disallowed peers, and requests when the HTTP work queue exceeds
  `-rpcworkqueue`, returning `503` on queue saturation. During shutdown,
  `InterruptHTTPServer()` switches new requests to rejection mode and
  `StopHTTPServer()` waits for tracked active connections to drain.
- `src/httprpc.cpp` (`HTTPReq_JSONRPC`) is the JSON-RPC HTTP gate. It accepts
  only `POST`, requires an `Authorization` header using the `Basic` scheme, and
  returns `401` plus `WWW-Authenticate` on missing or failed auth. Incorrect
  passwords incur a 250 ms sleep before reply.
- `src/httprpc.cpp` (`InitRPCAuthentication`) defines which credentials are
  valid at startup. It can use auth cookies, plaintext `-rpcuser/-rpcpassword`,
  and hashed `-rpcauth`. Invalid `-rpcauth` or `-rpccookieperms` causes RPC
  startup to fail instead of weakening runtime checks.
- `src/httprpc.cpp` (`ExecuteHTTPRPC`) applies `-rpcwhitelist` and
  `-rpcwhitelistdefault` after authentication. This is a per-user method policy
  that returns `403`; it is not wallet routing. For batched calls, a
  whitelisted user must be allowed to call every requested method or the batch
  is rejected before execution.
- `src/rpc/server.cpp` (`CRPCTable::execute`) dispatches only by
  `request.strMethod`. Warmup is a hard global gate (`RPC_IN_WARMUP`), and
  unknown methods fail with `RPC_METHOD_NOT_FOUND`.
- In the consulted sources, later dispatch does not apply a second per-wallet
  authorization check keyed by `authUser`. Wallet-aware handlers instead call
  `src/wallet/rpc/util.cpp` (`GetWalletNameFromJSONRPCRequest`,
  `GetWalletForJSONRPCRequest`) to interpret `request.URI`.
- `src/wallet/rpc/util.cpp` (`GetWalletNameFromJSONRPCRequest`) treats
  `/wallet/<walletname>` as wallet selection and URL-decodes the remainder.
  `GetWalletForJSONRPCRequest` resolves that loaded wallet, otherwise falls
  back to the default-wallet rules: one loaded wallet is selected implicitly,
  zero loaded wallets return `RPC_WALLET_NOT_FOUND`, and multiple loaded
  wallets return `RPC_WALLET_NOT_SPECIFIED`.
- `src/wallet/rpc/util.cpp` (`EnsureWalletIsUnlocked`) is a later wallet-method
  gate for encrypted wallets. It protects specific operations, but it is not
  part of HTTP ingress or RPC authentication.
- Privacy and availability implications grounded in these sources:
  - `src/httpserver.cpp` (`http_request_cb`) logs the sanitized request URI
    under `BCLog::HTTP`, so wallet names carried in `/wallet/<walletname>` can
    appear in HTTP debug logs.
  - `src/httprpc.cpp` (`HTTPReq_JSONRPC`) logs failed password attempts with
    the peer address.
  - `src/httpserver.cpp` (`HTTPBindAddresses`) warns when RPC is bound to a
    wildcard address because the interface is not safe to expose to untrusted
    networks.

## Important Code Paths

- HTTP ingress, allowlist, binding, queue backpressure, and shutdown:
  `src/httpserver.cpp` (`InitHTTPAllowList`, `HTTPBindAddresses`,
  `http_request_cb`, `InterruptHTTPServer`, `StopHTTPServer`)
- JSON-RPC HTTP auth, credential initialization, and method whitelist checks:
  `src/httprpc.cpp` (`InitRPCAuthentication`, `RPCAuthorized`,
  `HTTPReq_JSONRPC`, `ExecuteHTTPRPC`, `StartHTTPRPC`)
- Global RPC dispatch and warmup gating:
  `src/rpc/server.cpp` (`JSONRPCExec`, `CRPCTable::execute`, `ExecuteCommand`,
  `SetRPCWarmupStarting`, `SetRPCWarmupFinished`)
- Wallet name extraction and wallet selection:
  `src/wallet/rpc/util.cpp` (`GetWalletNameFromJSONRPCRequest`,
  `GetWalletForJSONRPCRequest`, `EnsureUniqueWalletName`,
  `EnsureWalletIsUnlocked`)

## Related Tests

- `test/functional/rpc_bind.py` exercises loopback defaults and the
  `-rpcallowip`/`-rpcbind` interaction.
- `test/functional/rpc_users.py` covers successful and failed auth,
  malformed `-rpcauth`, cookie-file behavior, and `-rpccookieperms`.
- `test/functional/rpc_whitelist.py` covers per-user method whitelists and the
  effect of `-rpcwhitelistdefault`.
- `test/functional/interface_rpc.py` covers HTTP status mapping for JSON-RPC
  requests, batch behavior, `getrpcinfo`, and `-rpcworkqueue` saturation.
- `test/functional/feature_shutdown.py` covers shutdown while RPC work is still
  active.
- `test/functional/wallet_multiwallet.py` covers wallet endpoint selection,
  the zero-wallet and multiwallet error paths, and mixed batch use of wallet
  and non-wallet methods through a wallet-specific endpoint.
- `src/wallet/test/wallet_rpc_tests.cpp` covers endpoint-versus-parameter
  consistency in `EnsureUniqueWalletName`.

## Adjacent Pages

- `[[areas/rpc-rest-zmq-and-interfaces]]`
- `[[areas/wallet]]`
- `[[concepts/operator-privacy]]`
- `[[concepts/resource-exhaustion-and-backpressure]]`
- `[[workflows/rpc-request-handling]]`
- `[[files/src/httprpc.cpp]]`

## Open Questions

- I did not find a focused test in the consulted set for the `POST`-only
  rejection path in `src/httprpc.cpp` (`HTTPReq_JSONRPC`).
- I did not find a focused test in the consulted set for the warmup rejection
  path in `src/rpc/server.cpp` (`CRPCTable::execute`) or for the absence of
  the `/wallet/` handler when wallet support is unavailable.

## Sources Consulted

- `src/httpserver.h`
- `src/httpserver.cpp`
- `src/httprpc.cpp`
- `src/rpc/server.h`
- `src/rpc/server.cpp`
- `src/wallet/rpc/util.cpp`
- `test/functional/feature_shutdown.py`
- `test/functional/interface_rpc.py`
- `test/functional/rpc_bind.py`
- `test/functional/rpc_users.py`
- `test/functional/rpc_whitelist.py`
- `test/functional/wallet_multiwallet.py`
- `src/wallet/test/wallet_rpc_tests.cpp`
