---
kind: workflow
title: Node Startup and Shutdown
status: active
last_reviewed: 2026-04-21
paths:
  - src/bitcoind.cpp
  - src/init.cpp
  - src/init/common.cpp
  - src/node/chainstate.cpp
  - src/node/kernel_notifications.h
  - src/node/kernel_notifications.cpp
  - src/node/miner.cpp
  - src/httpserver.cpp
  - src/rpc/server.cpp
  - src/rest.cpp
  - src/net.cpp
  - src/index/base.cpp
tags:
  - init
  - shutdown
  - warmup
  - scheduler
  - interrupt
---

# Node Startup and Shutdown

## Summary

This page describes the headless `bitcoind` path, centered on
`src/bitcoind.cpp` (`main`, `AppInit`) and `src/init.cpp`
(`AppInitMain`, `Interrupt`, `Shutdown`).

Startup installs a process-wide `util::SignalInterrupt`, performs basic setup
and sanity checks, daemonizes if requested, locks the data directories, starts
logging and the scheduler, optionally starts HTTP/RPC in warmup mode, loads
chainstate and optional indexes and chain clients, runs block import/index sync
and mempool load on `background_init_thread`, waits until a tip exists, starts
network threads, and only then clears RPC warmup with `SetRPCWarmupFinished()`.

Shutdown uses the same `SignalInterrupt`. The main thread in
`src/bitcoind.cpp` waits on `node.shutdown_signal->wait()`, then calls
`Interrupt(node)` to wake long-running subsystems and `Shutdown(node)` to stop
RPC/network threads, join background work, flush validation callbacks and
indexes, persist mempool and fee state, flush chainstate, remove the PID file,
and destroy node-owned objects.

## Responsibilities or invariants

- `src/init.cpp` (`InitContext`) creates a single process-wide
  `util::SignalInterrupt` (`g_shutdown`) and stores both
  `NodeContext::shutdown_signal` and `NodeContext::shutdown_request`. The same
  interrupt object is passed into `ChainstateManager` and `BlockManager` in
  `src/init.cpp` (`InitAndLoadChainstate`), so shutdown is visible to long
  chainstate-loading and verification work.
- `src/util/signalinterrupt.h` and `src/util/signalinterrupt.cpp`
  (`util::SignalInterrupt`) are signal-handler-safe. On Unix,
  `src/init.cpp` (`AppInitBasicSetup`) installs `SIGTERM`/`SIGINT` handlers
  that only trigger the interrupt; `main` later does the blocking wait in a
  normal thread context.
- `src/init.cpp` (`AppInitMain`) owns the startup order for PID/logging,
  scheduler startup, validation callback plumbing, warnings/notifications,
  chainstate loading, optional indexes, wallet and other chain clients, and
  finally P2P startup.
- Wallet startup is also staged deliberately. `src/init.cpp` drives the
  generic chain-client lifecycle so wallet verification happens before network
  start, wallet DB loading happens after chainstate and index setup, and
  wallet post-init work is deferred until late startup
  (`src/wallet/load.cpp` (`VerifyWallets`, `LoadWallets`, `StartWallets`)).
- Validation callbacks are serialized through
  `src/init.cpp` (`AppInitMain`) creating `ValidationSignals` with
  `SerialTaskRunner(*node.scheduler)`. The ordering guarantee comes from
  `src/scheduler.h` (`SerialTaskRunner`) and `src/validationinterface.h` /
  `src/validationinterface.cpp` (`ValidationSignals`): each subscriber sees its
  callbacks in-order, but not on a dedicated thread.
- The scheduler thread starts early and is shared by validation callbacks,
  periodic maintenance, and post-start scheduled tasks. `src/init.cpp`
  (`Shutdown`) explicitly warns that after `node.scheduler->stop()`,
  `SyncWithValidationInterfaceQueue()` must not be called anymore, because that
  can prevent shutdown from completing.
- If `-server=1`, `src/init.cpp` (`AppInitServers`) starts HTTP/RPC before
  chainstate loading completes, but RPC remains in warmup until
  `src/init.cpp` (`AppInitMain`) calls `SetRPCWarmupFinished()` after
  `node.connman->Start(...)` succeeds and the RPC view of the best block is
  consistent with the active tip. If `-server=0`, `AppInitServers` is skipped
  and there is no external HTTP/RPC surface. `src/rest.cpp` (`CheckWarmup`)
  returns HTTP 503 during warmup.
- `bitcoind` soft-sets `-server=1` in `src/bitcoind.cpp` (`AppInit`), but the
  workflow still depends on runtime gates: `-daemon` / `-daemonwait`,
  `-server`, `-rest`, `-reindex`, `-reindex-chainstate`, `-txindex`,
  `-blockfilterindex`, `-coinstatsindex`, `-txospenderindex`,
  `-stopafterblockimport`, `-stopatheight`, `-persistmempool`, `-natpmp`,
  `-startupnotify`, and `-shutdownnotify`.
- Two crash/offline guards are wired directly into startup and shutdown:
  `src/init.cpp` (`AppInitMain`) schedules a recurring disk-space check that
  requests shutdown rather than risking database corruption, and
  `src/node/abort.cpp` (`AbortNode`) converts fatal internal errors into
  `EXIT_FAILURE` plus a shutdown request.
- `src/init.cpp` (`ShutdownNotify`) runs `-shutdownnotify` synchronously before
  shutdown interrupts are sent. The arg help text in `src/init.cpp`
  (`SetupServerArgs`) explicitly warns that this can delay urgent shutdown.

## Important code paths

### 1. Process bootstrap and shutdown signal wiring

1. `src/bitcoind.cpp` (`main`) creates `NodeContext`, calls
   `interfaces::MakeNodeInit(...)`, sets up the environment, parses args, and
   calls `AppInit(node)`.
2. `src/init.cpp` (`InitContext`) creates `g_shutdown`, stores it in
   `node.shutdown_signal`, and exposes `node.shutdown_request` as the common
   shutdown trigger used by signal handlers, RPC, fatal error paths, and
   background startup tasks.
3. `src/init.cpp` (`AppInitBasicSetup`) installs signal handlers, ignores
   `SIGPIPE` on Unix, and performs basic networking setup. `src/bitcoind.cpp`
   (`main`) later waits on `node.shutdown_signal->wait()`.
4. `src/bitcoind.cpp` (`main`) always calls `Interrupt(node)` and
   `Shutdown(node)` after the wait returns, even if initialization failed and
   `exit_status` is already being forced to failure.

### 2. Pre-flight startup before chainstate loading

1. `src/bitcoind.cpp` (`AppInit`) sets `-server=1` by default, initializes
   logging early enough for parameter-interaction errors to reach the console,
   runs `AppInitBasicSetup`, `AppInitParameterInteraction`, and
   `AppInitSanityChecks`, optionally daemonizes for `-daemon` /
   `-daemonwait`, locks the data and blocks directories with
   `AppInitLockDirectories`, then calls `AppInitInterfaces(node)` and
   `AppInitMain(node)`.
   With `-daemonwait=0`, the child signals startup success to the parent
   immediately after fork; with `-daemonwait=1`, the parent does not exit until
   initialization has actually finished.
2. `src/init.cpp` (`AppInitMain`) creates the PID file, starts full logging via
   `src/init/common.cpp` (`init::StartLogging`), constructs `node.scheduler`,
   starts the `scheduler` service thread, and schedules recurring entropy
   gathering and disk-space checks.
3. `src/init.cpp` (`AppInitMain`) then creates `ValidationSignals`,
   `KernelNotifications`, wallet and other chain-client interfaces, registers
   RPC commands, validates listen/bind/network options, and sets up reachable
   network state before any P2P thread is started.

### 3. Warmup HTTP/RPC startup and interruptible chainstate load

1. If `-server=1`, `src/init.cpp` (`AppInitServers`) calls
   `src/httpserver.cpp` (`InitHTTPServer`, `StartHTTPServer`),
   `src/rpc/server.cpp` (`StartRPC`), `src/httprpc.cpp` (`StartHTTPRPC`), and
   optionally `src/rest.cpp` (`StartREST`). At this point the server can accept
   connections, but RPC and REST are still in warmup.
2. `src/init.cpp` (`InitAndLoadChainstate`) resets any prior partially loaded
   chainstate state, clears `KernelNotifications::chainstate_loaded`, rebuilds
   `node.mempool`, constructs `node.chainman`, and calls
   `src/node/chainstate.cpp` (`LoadChainstate`, `VerifyLoadedChainstate`).
3. `src/node/chainstate.cpp` (`LoadChainstate`, `VerifyLoadedChainstate`)
   returns `ChainstateLoadStatus::INTERRUPTED` when long verification work is
   interrupted. This is driven by the shared shutdown signal because
   `src/validation.cpp` (`ChainstateManager::ChainstateManager`) stores the
   same `util::SignalInterrupt` reference in `ChainstateManager::m_interrupt`.
4. On success, `src/init.cpp` (`InitAndLoadChainstate`) sets
   `node.notifications->setChainstateLoaded(true)`, creates `PeerManager`,
   initializes optional indexes gated by `-txindex`, `-txospenderindex`,
   `-blockfilterindex`, and `-coinstatsindex`, and loads chain clients.
5. If chainstate initialization fails before shutdown is requested, the
   `src/init.cpp` (`AppInitMain`) GUI retry path may rerun
   `InitAndLoadChainstate(...)` once with reindexing enabled.

### 4. Background init, tip readiness, and leaving warmup

1. `src/init.cpp` (`AppInitMain`) starts `background_init_thread`
   (`initload`). That thread runs `ImportBlocks(...)`, updates IBD state,
   requests shutdown on `-stopafterblockimport`, starts index background sync
   via `StartIndexBackgroundSync(node)`, and loads the mempool from disk when
   `ShouldPersistMempool(args)` is true.
2. The main startup thread waits on
   `src/node/kernel_notifications.h` (`KernelNotifications::m_tip_block_cv`)
   until `KernelNotifications::TipBlock()` becomes non-null or shutdown is
   requested. `src/node/kernel_notifications.cpp`
   (`KernelNotifications::blockTip`) sets the cached tip and notifies the
   condition variable whenever the validated tip changes.
3. This wait is important on first startup and reindex paths: the comment in
   `src/init.cpp` (`AppInitMain`) notes that when no prior chainstate exists,
   the first connected tip may only arrive from `ActivateBestChain()` running
   inside `background_init_thread`.
4. After a tip exists, `src/init.cpp` (`AppInitMain`) records best tip/header
   info, starts NAT-PMP via `StartMapPort(...)` when `-natpmp=1`, builds
   `CConnman::Options`, and calls `node.connman->Start(scheduler, connOptions)`.
   `src/net.cpp` (`CConnman::Start`) then starts the long-lived network
   threads (`net`, `dnsseed`, `addcon`, `opencon`, `msghand`, and optional
   `i2paccept` / `privbcast`) and schedules peer-address and ASMap maintenance
   on the shared scheduler.
5. Only after `node.connman->Start(...)` succeeds does `src/init.cpp`
   (`AppInitMain`) call `SetRPCWarmupFinished()`. It then starts chain clients
   with `client->start(scheduler)`, schedules banlist dumping and peer tasks,
   and runs `-startupnotify` asynchronously via `StartupNotify(args)`.

### 5. Shutdown request sources and interrupt stage

- Shutdown requests can originate from multiple places that all converge on
  `node.shutdown_request`:
  - `src/init.cpp` (`HandleSIGTERM`, `consoleCtrlHandler`) for external
    process signals
  - `src/rpc/server.cpp` (`stop`) for the RPC `stop` method
  - `src/init.cpp` (`AppInitMain`) recurring disk-space checks
  - `src/init.cpp` (`AppInitMain`) `-stopafterblockimport`
  - `src/node/kernel_notifications.cpp` (`KernelNotifications::blockTip`) for
    `-stopatheight`
  - `src/node/abort.cpp` (`AbortNode`) and
    `src/node/kernel_notifications.cpp` (`flushError`, `fatalError`) for fatal
    internal errors
- `src/init.cpp` (`Interrupt`) is the first ordered teardown step after the
  main thread wakes. It runs `-shutdownnotify`, wakes
  `KernelNotifications::m_tip_block_cv`, interrupts HTTP, HTTP-RPC, RPC, REST,
  the Tor controller, NAT-PMP, `CConnman`, and every enabled index.
- `src/rpc/server.cpp` (`InterruptRPC`, `RpcInterruptionPoint`) flips
  `g_rpc_running` to false exactly once. RPC code that checks
  `RpcInterruptionPoint()` will then fail with `RPC_CLIENT_NOT_CONNECTED` and
  the `"Shutting down"` message.
- Long waits for tip changes are also shutdown-aware. `src/node/miner.cpp`
  (`WaitTipChanged`) waits on `KernelNotifications::m_tip_block_cv` but returns
  `nullopt` if `chainman.m_interrupt` becomes true. Because
  `ChainstateManager::m_interrupt` is the shared shutdown signal, long-poll RPCs
  like `waitfornewblock` and `waitforblockheight` return promptly on shutdown
  instead of waiting for their full timeout.

### 6. Ordered teardown, persistence, and object destruction

1. `src/init.cpp` (`Shutdown`) is single-entry via `g_shutdown_mutex` and is
   written to tolerate partial initialization failures.
2. It stops inbound control planes first:
   `StopHTTPRPC()`, `StopREST()`, `StopRPC()`, and `StopHTTPServer()`.
   `src/httpserver.cpp` (`StopHTTPServer`) stops worker threads, unlistens
   sockets, waits for active HTTP connections to drain, and only then joins the
   HTTP event thread. This ordering preserves the reply path for the `stop` RPC
   and prevents new inbound work from racing teardown.
3. It stops chain clients, NAT-PMP, unregisters `peerman` from validation
   signals, stops `connman`, joins the Tor controller, and joins
   `background_init_thread`.
4. Only after producers are stopped does it stop the shared scheduler. The
   explicit shutdown comment says that calling
   `SyncWithValidationInterfaceQueue()` after this point can block shutdown.
5. With peers and RPC stopped, shutdown persists mutable node state:
   `DumpMempool(...)` if mempool persistence is enabled and loading was
   attempted, `fee_estimator->Flush()`, and `ForceFlushStateToDisk()` for each
   flushable chainstate under `::cs_main`.
6. `src/init.cpp` (`Shutdown`) then calls
   `validation_signals->FlushBackgroundCallbacks()`, stops each index, and
   clears the global index singletons. `src/index/base.cpp` (`BaseIndex::Stop`)
   first unregisters from validation signals and then joins the index sync
   thread, so index teardown happens after queued validation callbacks have
   been drained.
7. Finally shutdown force-flushes chainstate again and resets coin views,
   disconnects incoming IPC clients, unregisters ZMQ and remaining validation
   interfaces, destroys mempool/fee-estimator/chainman/scheduler/kernel/ECC
   state, removes the PID file, and logs `"Shutdown done"`.

## Related tests

- `src/test/node_init_tests.cpp` (`init_test`) runs `AppInitInterfaces`,
  `AppInitMain`, `Interrupt`, and `Shutdown` directly in a unit-test setup.
- `test/functional/feature_init.py` (`init_stress_test_interrupt`) repeatedly
  terminates the node at different startup log milestones and checks that later
  restarts still succeed.
- `test/functional/feature_init.py` (`break_wait_test`) checks that a signal
  during `waitforblockheight` interrupts the RPC promptly instead of waiting
  for the full timeout.
- `test/functional/feature_shutdown.py` checks shutdown while a long-poll
  `waitfornewblock` RPC is active and relies on the HTTP server waiting for
  current connections to close.
- `test/functional/feature_startupnotify.py` checks that `-startupnotify` runs
  once after the node is fully started.
- `test/functional/feature_notifications.py` checks that `-shutdownnotify`
  fires during node shutdown.
- `test/functional/feature_abortnode.py` exercises a fatal internal error path
  (`AbortNode`) and verifies process exit plus failed restart.

## Adjacent pages

- `[[areas/common-utils-and-configuration]]`
- `[[areas/validation-and-chainstate]]`
- `[[workflows/rpc-request-handling]]`
- `[[workflows/block-validation-and-connection]]`
- `[[workflows/initial-block-download]]`
- `[[files/src/init.cpp]]`
- `[[files/src/wallet/load.cpp]]`

## Open questions

- This page is intentionally `bitcoind`-centric. The Qt path in
  `src/qt/bitcoin.cpp` uses `interfaces::Node::startShutdown()` and a Qt timer
  polling `ShutdownRequested()`, which is closely related but not documented
  here.
- `src/init.cpp` still has older comments referring to a network-processing
  “thread group”, while the current tree uses several separately owned threads
  (`scheduler`, `initload`, `CConnman` threads, per-index sync threads). A
  dedicated thread-ownership page could make those boundaries easier to review.

## Sources consulted

- `src/bitcoind.cpp` (`main`, `AppInit`)
- `src/init.cpp` (`InitContext`, `AppInitBasicSetup`, `AppInitParameterInteraction`, `AppInitInterfaces`, `InitAndLoadChainstate`, `AppInitMain`, `Interrupt`, `Shutdown`, `AppInitServers`)
- `src/init/common.cpp` (`init::StartLogging`)
- `src/util/signalinterrupt.h`, `src/util/signalinterrupt.cpp`
- `src/node/chainstate.cpp` (`LoadChainstate`, `VerifyLoadedChainstate`)
- `src/validation.cpp` (`ChainstateManager::ChainstateManager`)
- `src/node/kernel_notifications.h`, `src/node/kernel_notifications.cpp`
- `src/node/miner.cpp` (`WaitTipChanged`, `InterruptWait`)
- `src/httpserver.cpp` (`InitHTTPServer`, `StartHTTPServer`, `InterruptHTTPServer`, `StopHTTPServer`)
- `src/httprpc.cpp` (`StartHTTPRPC`)
- `src/rpc/server.cpp` (`stop`, `StartRPC`, `InterruptRPC`, `StopRPC`, `SetRPCWarmupFinished`, `RPCIsInWarmup`)
- `src/rest.cpp` (`CheckWarmup`, `StartREST`, `InterruptREST`, `StopREST`)
- `src/net.cpp` (`CConnman::Start`, `CConnman::Interrupt`, `CConnman::Stop`)
- `src/index/base.cpp` (`BaseIndex::Init`, `BaseIndex::Interrupt`, `BaseIndex::StartBackgroundSync`, `BaseIndex::Stop`)
- `src/validationinterface.h`, `src/validationinterface.cpp`
- `src/node/abort.cpp` (`AbortNode`)
- `src/test/node_init_tests.cpp`
- `test/functional/feature_init.py`
- `test/functional/feature_shutdown.py`
- `test/functional/feature_startupnotify.py`
- `test/functional/feature_notifications.py`
- `test/functional/feature_abortnode.py`
