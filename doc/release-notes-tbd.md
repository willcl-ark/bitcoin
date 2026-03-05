P2P and network changes
-----------------------

- Manual peers (added using `addnode` or `-connect`) are no longer disconnected
  during IBD for block stalling, block download timeout, or headers sync timeout.
  For block stalling/download timeout cases, in-flight block requests from manual
  peers are released so other peers can continue block download progress.
