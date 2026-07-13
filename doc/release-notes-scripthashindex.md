New settings
------------

- A new `-scripthashindex` startup option has been added. When enabled, the
  node maintains a compact index of confirmed transaction activity by script
  hash. The index can be used to look up confirmed history, unspent outputs,
  and confirmed balances for a script hash.

New RPCs
--------

- The `getscripthashactivity`, `getscripthashhistory`, `getscripthashutxos`,
  and `getscripthashbalance` RPCs have been added. These RPCs are intended for
  local services that need script-hash-indexed chain data, such as an external
  protocol adapter, without requiring Bitcoin Core to serve that protocol
  directly.

  These RPCs require the node to be started with `-scripthashindex=1`. The
  index is not available in prune mode, and the returned data is limited to
  confirmed chain state. History and UTXO results are returned in full and are
  not paginated by Bitcoin Core, so callers should avoid exposing these RPCs to
  untrusted clients without their own result limiting.

  When both `-scripthashindex=1` and `-txospenderindex=1` are enabled, the
  script hash index reuses the transaction output spender index for confirmed
  spend lookups instead of storing duplicate spend rows.
