# Notifications

Bitcoin Core can notify external software about node, chain, mempool, wallet,
and warning events. Notifications are useful for waking up external processes,
but they are not a substitute for querying the node for current state. When a
notification is received, use the RPC interface to confirm the state that is
relevant to the application.

## Command notifications

Command notification options execute a command line configured with the
corresponding option. Commands are executed by the operating system shell.
Applications should not depend on commands finishing in the same order in which
events were generated. If a command exits with an error, the error is logged, but
it is not reported through RPC.

Except for `-shutdownnotify`, command notification options use a single
configured command. If the same option is provided more than once, the usual
configuration precedence rules select the value that is used. `-shutdownnotify`
can be provided multiple times, and each configured command is executed during
shutdown.

Applications should treat command notifications as at-least-once hints. Duplicate
notifications can happen, and applications should make their notification
handling idempotent.

### `-blocknotify`

`-blocknotify=<cmd>` executes `<cmd>` when the best block changes. `%s` in the
command is replaced by the block hash.

The command is started asynchronously. The notification indicates that the node
observed a best-block update, but by the time the command runs the best block
may already have changed again. Use RPCs such as `getbestblockhash` and
`getblock` if the application needs current chain state.

`-blocknotify` is only run after the node has completed its initial block index
or block download state. It is not run for block tip updates while the node is
reindexing or still in initial block download.

`-blocknotify` is not synchronized with ZMQ block notifications or internal
validation-interface callbacks. These mechanisms can describe related best-chain
events, but applications should not assume they are delivered together or in the
same order.

### `-walletnotify`

`-walletnotify=<cmd>` executes `<cmd>` when a wallet transaction changes. `%s`
is replaced by the transaction id, `%w` by the wallet name on systems where that
placeholder is supported, `%b` by the containing block hash or `unconfirmed`,
and `%h` by the containing block height or `-1`.

Wallet notifications can be triggered when transactions become unconfirmed,
confirmed, or are found during rescans. The command is started asynchronously.
Use wallet RPCs such as `gettransaction` or `listtransactions` to confirm the
wallet's current view of the transaction.

When a wallet transaction is removed from the mempool because it conflicts with a
transaction in a connected block, the wallet may emit an unconfirmed notification
for the removed transaction before later marking it conflicted. Applications
should query the wallet after receiving a notification instead of relying only on
the notification's placeholder values.

### `-alertnotify`

`-alertnotify=<cmd>` executes `<cmd>` when the node raises a warning. `%s` is
replaced by the warning message. A warning that is cleared and later raised
again can trigger another notification.

### `-startupnotify`

`-startupnotify=<cmd>` executes `<cmd>` after startup. The command is started
asynchronously.

### `-shutdownnotify`

`-shutdownnotify=<cmd>` executes `<cmd>` immediately before shutdown begins.
Shutdown may be urgent, so configured commands should return quickly or fork
their long-running work into the background.

Unlike other command notification options, all configured `-shutdownnotify`
commands are started and Bitcoin Core waits for them to finish before continuing
shutdown.

## ZMQ notifications

ZMQ notifications publish block, transaction, and sequence messages to
subscribers. See [ZMQ](zmq.md) for configuration, message formats, topic names,
and detailed topic behavior.

ZMQ is a publish-subscribe interface. Subscribers can miss messages if they are
not connected, cannot keep up, or exceed the publisher's high water mark. ZMQ
messages include a sequence number for each configured notifier stream that
subscribers can use to detect missed messages. Subscribers should also handle
duplicate transaction notifications, because the same transaction can be
published when it enters the mempool, when it appears in a connected block, and
when that block is disconnected during a reorganization.

For a single ZMQ notifier, published messages follow the order in which the node
processes the corresponding validation and mempool events. Subscribers should
not assume that receiving a ZMQ message is synchronized with an RPC response
unless that RPC explicitly documents such synchronization. If exact current
state matters, use RPC after receiving the notification.

## RPC consistency

RPC calls return a view of the node at the time the individual RPC is handled.
Bitcoin Core does not generally provide a snapshot that spans multiple RPC
calls. For example, if a client calls `getbestblockhash` and then calls
`getrawmempool`, the mempool result is not guaranteed to be the mempool as it
existed at the earlier block hash. It is a later view of the mempool.

During reorgs, applications should not rely on mempool RPCs to expose every
intermediate mempool state. A mempool RPC returns the mempool view available when
that RPC is handled, and later calls may reflect later reorg progress.

If an application needs to track both chain and mempool changes, it should treat
notifications as hints and use RPCs to reconcile state. The `getrawmempool` RPC
can return a `mempool_sequence` value that can be compared with ZMQ `sequence`
notifications to detect missed mempool updates.

## Validation interface callbacks

Internal validation-interface subscribers receive callbacks in the order events
were generated for that subscriber, and callbacks for a subscriber are invoked
serially. No ordering should be assumed across different subscribers.

This internal callback ordering does not imply that separate external
notification mechanisms are synchronized with each other. For example, an
application should not assume a shell notification, a ZMQ notification, and a
wallet RPC response all describe the same snapshot unless it explicitly checks
the relevant state.
