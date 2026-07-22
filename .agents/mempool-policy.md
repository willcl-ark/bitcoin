# Mempool and policy

This guide covers Bitcoin Core mempool and relay policy development conventions. It complements the canonical documentation at `doc/policy/` and `doc/mempool.md`.

## Design principles

- Keep mempool behavior, consensus rules, and mining policy conceptually distinct. Policy changes that restrict default relay require focused tests, clear rationale, and public discussion.
- Maintain separate parameters for relay policy (`-minrelaytxfee`) and mining policy (`-blockmintxfee`). To loosen policy, relax relay first then mining; to tighten, restrict mining first then relay. In steady state the two should match. The default `-blockmintxfee` must never exceed the default `-minrelaytxfee`, or a node would relay transactions it will never mine (free relay problem).
- Preserve established option semantics. When new restrictions are needed, introduce new configuration options rather than redefining the scope of existing ones (e.g., `-datacarriersize`).

## Review and merging

- Mempool policy proposals do not require a BIP as a prerequisite for merging. Documentation can be maintained alongside the code and formalized afterward if desired.
- PRs adding disabled-by-default relay policy options are acceptable when the code is small, self-contained, maintainable, and demand is evident. Changing **default** policy requires clear evidence that a majority of hashrate would adopt the change.
- Before merging a PR that restricts Bitcoin Core's default relay or mempool policy, post a corresponding announcement to the bitcoin-dev mailing list for community discussion.

## Implementation patterns

- Maintain two separate reject caches: `m_recent_rejects` (permanent rejections) and `m_recent_rejects_reconsiderable` (rejections due to fee reasons that could be overcome via package submission such as CPFP).
- Use `FeeFrac` exact fraction arithmetic rather than approximate `CFeeRate` for mempool and mining fee rate comparisons. Round up in fee estimation contexts; round down (or use exact arithmetic) in mining/mempool sorting contexts.
- Enforce invariants on configurable constants at compile time with `static_assert` when the constant must lie within a specific range for correctness.
- Structure mempool event log messages (e.g., replacement logs) to be script-friendly: log all replaced transactions first, then log the replacement once.

## Testing

- When introducing new policy rules, include fuzz testing by amending existing fuzz targets (`tx_pool`, `tx_package_eval`) or creating a standalone harness.
- Add functional tests covering both valid cases and boundary conditions (e.g., maximum standard transaction size, maximum cluster conflicts for package RBF).
- Document in code comments when a condition is necessary but not sufficient for correctness, and add inline comments explaining non-obvious reasoning (e.g., why a sibling is marked as a mempool conflict).

## Documentation

- Document mempool policy fee and size terminology clearly in `doc/policy/`, covering: weight (BIP 141), sigops-adjusted virtual size, sigops-adjusted weight, base fee vs modified fee (with `prioritisetransaction` deltas).
- Refer to the canonical `doc/policy/` directory and `doc/mempool.md` for the authoritative reference on current policy rules.
