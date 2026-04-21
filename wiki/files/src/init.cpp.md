---
kind: file
title: src/init.cpp
status: active
last_reviewed: 2026-04-21
paths:
  - src/init.cpp
tags:
  - init
  - startup
  - shutdown
  - availability
  - rpc
---

# src/init.cpp

## Role in the System

`src/init.cpp` is the node runtime bootstrap and teardown coordinator. It owns
the `NodeContext` shutdown signal (`InitContext()`), the main post-argument
startup sequence (`AppInitMain()`), and the ordered teardown path
(`Interrupt()` then `Shutdown()`).

For review, this file is a high-value availability boundary: it decides whether
partial initialization fails cleanly, whether RPC exits warmup only after the
node is actually ready, and whether shutdown stops producers before flushing
and destroying shared state.

## Important Types and Functions

- `InitContext()` installs the process-wide `util::SignalInterrupt` into
  `node::NodeContext` as `shutdown_signal` and `shutdown_request`.
- `ShutdownRequested()` is the common readiness/interrupt check used during
  startup waits and long-running initialization.
- `Interrupt()` is the stop-signal fanout path. It wakes tip waiters and
  interrupts HTTP, RPC, REST, Tor, port mapping, `CConnman`, and all indexes.
- `Shutdown()` is the ordered teardown path. It is explicitly written to handle
  partial initialization, stops RPC/HTTP ingress first, joins the background
  init thread, stops the scheduler, persists the mempool if appropriate,
  flushes validation callbacks, stops indexes, flushes chainstates, clears
  kernel/runtime objects, and removes the PID file.
- `AppInitSanityChecks()` performs kernel sanity checks, `ECC_InitSanityCheck`,
  and pre-daemonization directory-lock probes.
- `AppInitLockDirectories()` reacquires and holds the real datadir/blocksdir
  locks after daemonization.
- `AppInitServers()` starts HTTP/RPC/REST early, while RPC remains in warmup.
  `AppInitMain()` clears warmup with `SetRPCWarmupFinished()` only after
  chainstate load, peer manager setup, network start, and best-tip publication.
- `InitAndLoadChainstate()` constructs and loads `CTxMemPool` and
  `ChainstateManager`, and resets `node.notifications`, `node.mempool`, and
  `node.chainman` before a retry.
- `StartIndexBackgroundSync()` refuses startup when an index's required block
  or undo data has been pruned away.

## Callers and Dependencies

- `src/bitcoind.cpp` (`AppInit()` and `main()`) is the direct daemon caller:
  it runs `AppInitInterfaces()` and `AppInitMain()`, waits on
  `node.shutdown_signal`, then calls `Interrupt()` and `Shutdown()`.
- `AppInitServers()` depends on `src/httpserver.cpp`, `src/httprpc.cpp`,
  `src/rest.cpp`, and `src/rpc/server.cpp` (`RpcInterruptionPoint`,
  `SetRPCWarmupStatus()`, `SetRPCWarmupFinished()`).
- Startup creates `CScheduler`, `ValidationSignals`, and
  `node::KernelNotifications` before chainstate loading, then wires in
  wallets, `BanMan`, `AddrMan`, `NetGroupManager`, `CConnman`, `PeerManager`,
  indexes, and the background import thread.
- Teardown depends on the reverse ordering: ingress is stopped before peers and
  callbacks are flushed; the background init thread is joined before the
  scheduler stops; validation callbacks are flushed before indexes are stopped
  and reset.

## Related Tests

- `src/test/node_init_tests.cpp` runs `AppInitInterfaces()`,
  `AppInitMain()`, `Interrupt()`, and `Shutdown()` as a basic startup/shutdown
  smoke test.
- `test/functional/interface_bitcoin_cli.py` exercises `-rpcwait`, covering
  the period where RPC is listening but still in warmup.
- `test/functional/feature_startupnotify.py` covers `StartupNotify()` after
  successful startup completion.
- `test/functional/feature_notifications.py` covers `-shutdownnotify`, which
  is dispatched from `Interrupt()`.
- `test/functional/mempool_persist.py` covers mempool dump/load behavior tied
  to `Shutdown()` and the background init thread's `LoadMempool()` path.
- `test/functional/feature_index_prune.py` covers the fatal startup failure
  path from `StartIndexBackgroundSync()` when indexes need pruned-away data.
- `test/functional/rpc_users.py` covers one `AppInitServers()` failure mode by
  asserting startup aborts when RPC auth configuration prevents HTTP server
  startup.

## Notes or Risks

- Critical categories:
  - `offline`: startup failures here can leave the node unable to serve P2P,
    RPC, or both.
  - `resource`: scheduler, background-init, mempool persistence, and chainstate
    flush ordering directly affect shutdown latency and disk safety.
  - `fatal`: many failures intentionally abort startup rather than continue in
    a degraded state.
- Verified behavior: RPC/HTTP can start before chainstate loading finishes, but
  warmup is not cleared until late in `AppInitMain()`. Review impact:
  availability bugs here can leave RPC reachable but permanently unusable, or
  expose callers to partially initialized state.
- Verified behavior: `Interrupt()` and `Shutdown()` are intentionally separate.
  Adding a new long-lived thread or service to startup without wiring it into
  both paths is a shutdown-hang risk.
- Verified behavior: `Shutdown()` is null-checked throughout because it must
  tolerate initialization failures after only part of the runtime graph exists.
  Review impact: new persistent or threaded resources added to startup should
  either be optional-safe here or fully owned by an existing object that is.
- Verified behavior: the background `initload` thread imports blocks, can
  request shutdown for `-stopafterblockimport`, starts index sync, and loads
  the mempool. `Shutdown()` joins this thread before stopping the scheduler and
  before tearing down the remaining runtime objects.
- Verified behavior: low disk space is treated as a shutdown condition both at
  startup (`CheckDiskSpace()` before import) and during steady state (a
  scheduler task triggers `shutdown_request` every 5 minutes).
- Verified behavior: `AppInitSanityChecks()` probes directory locks before
  daemonization, while `AppInitLockDirectories()` acquires the lasting locks
  afterward. The code comments note a small post-fork race window that should
  at worst cause exit without a console message, not concurrent use of the same
  datadir.
- Verified behavior: `StartIndexBackgroundSync()` fails closed if an enabled
  index would need missing pruned data. That prevents serving an apparently
  healthy node with silently broken index state.

## Sources Consulted

- `src/init.cpp`
- `src/init.h`
- `src/bitcoind.cpp`
- `src/rpc/server.cpp`
- `src/node/kernel_notifications.h`
- `src/node/kernel_notifications.cpp`
- `src/test/node_init_tests.cpp`
- `test/functional/interface_bitcoin_cli.py`
- `test/functional/feature_startupnotify.py`
- `test/functional/feature_notifications.py`
- `test/functional/mempool_persist.py`
- `test/functional/feature_index_prune.py`
- `test/functional/rpc_users.py`
