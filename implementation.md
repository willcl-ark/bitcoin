# Implementation Notes

## Native event-loop dispatcher

Added `ipc::capnp::EventLoopDispatcher` to marshal short Cap'n Proto I/O
callbacks back onto the KJ event-loop thread. It captures the current thread's
KJ executor, runs directly when already on the event-loop thread, and uses
`kj::Executor::executeSync()` when called from Bitcoin worker or validation
threads.

This is intentionally separate from `WorkerQueue`: `WorkerQueue` runs blocking
Bitcoin work away from the event-loop thread, while `EventLoopDispatcher` is
for Cap'n Proto client I/O that must occur on the event-loop thread.
`ServeNative()` now creates the dispatcher alongside the per-connection worker
queue so future callback adapters can avoid doing Cap'n Proto I/O from
arbitrary Bitcoin threads.
