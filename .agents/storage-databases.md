# Storage and databases

This guide covers serialization safety, database operation discipline, and format-compatibility constraints for Bitcoin Core's on-disk storage layers.

## Serialization and format boundaries

- Use **explicit-width integer types** (`uint32_t`, `int8_t`, etc.) in serialization code; platform-dependent types like plain `int` or `signed char` produce divergent layouts across toolchains and architectures.
- **Pin exact serialized encodings** with unit tests when changing key formats or hash types. A matching serializer/deserializer change can silently alter the persisted format if not caught by a test.
- When fixing a broken hash or serialization format, **rename the hash type** (e.g., bump `hash_serialized_2` → `hash_serialized_3`) and add a release note. Do not keep the broken version to avoid breaking changes.
- **Validate untrusted metadata** from external inputs (e.g., block height in UTXO snapshots) against a trusted source before use; remove redundant fields that only replicate consensus data.

## Database write operations

- **Check every write return value** — ignored failures silently lose data.
- **Release semaphores on all error paths** in database write paths. A semaphore that is taken but not released after a failed statement or transaction begin will block all future operations.
- Guard transaction wrappers against **duplicate `TxnBegin()` calls**: check whether a transaction is already in progress before acquiring a semaphore or beginning a new one, and throw a logic error if it occurs.

## Format changes and downgrade compatibility

- When a PR alters the database storage format, **document that downgrade requires `-reindex`** in release notes or the commit message. See [doc/release-process.md](../doc/release-process.md) for the expected format.
- After a format change, users who downgrade without running `-reindex` will encounter corruption or crashes — make this consequence explicit.

## LevelDB-specific notes

- **Review LevelDB upgrades** for changes in file-descriptor management and compaction behavior. Seek-triggered compaction causes write amplification for Bitcoin Core's random-key chainstate workload and can be disabled when bloom filters, cache, and SSDs are in use.
- **Drain pending work** in `DeterministicEnv` before destroying the database in fuzz tests to prevent hangs.

## Testing

- Run tests that exercise database code paths **on disk** when the production path differs from the in-memory path.
- For general test practices see [CONTRIBUTING.md](../CONTRIBUTING.md) and [src/test/README.md](../src/test/README.md).
