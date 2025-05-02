Block index storage
-------------------

- The block index in `blocks/index` now uses flat files instead of LevelDB.
  On the first startup, Bitcoin Core migrates the existing index and removes
  its LevelDB files after the new index has been written successfully. This
  migration also applies to pruned nodes. Back up the data directory before
  upgrading if you need to preserve the option of restoring the old index.

- Downgrading after migration requires `-reindex`. A pruned node must download
  its missing blocks again. `-reindex-chainstate` does not rebuild the block
  index and is insufficient for a downgrade.

Experimental kernel library
---------------------------

- `btck_block_tree_reader_create` opens a snapshot of the persisted block index
  without opening the chainstate database. It can enumerate headers and read
  block and spent-output data while Bitcoin Core is running. Create a new
  reader to load newly persisted headers. This interface does not expose the
  active chain, and reads can fail if the node prunes the referenced files.
  A completed write-ahead log must be recovered by a writer before a reader
  can open the store.

- The in-memory block-tree option,
  `btck_chainstate_manager_options_update_block_tree_db_in_memory`, has been
  removed. Chainstate managers now persist the block index even when their
  chainstate database is configured to reside in memory. Kernel clients should
  use a temporary data directory when they need disposable block-tree storage.
  The kernel API remains experimental and unstable.
