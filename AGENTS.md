# AGENTS.md

## Authority

Read [CONTRIBUTING.md](CONTRIBUTING.md), [developer notes](doc/developer-notes.md),
and the [AI policy](doc/AI_POLICY.md) before making project changes. Those files
are authoritative. The focused guides below summarize recurring rules; resolve
conflicts in favor of the canonical documents.

## Working rules

- Understand the existing code and be able to explain every change in your own
  words. AI output does not replace human ownership or review.
- Keep changes focused. Do not mix behavior changes with unrelated formatting,
  moves, or refactors.
- Update tests, documentation, and release notes when behavior requires them.
- Keep each commit atomic, independently buildable, and testable. Explain the
  rationale in the commit message; do not use `@` mentions.
- Use human-written maintainer communication. Do not let an autonomous agent
  open or drive pull requests, or generate reviewer replies.

## Area guidance

Read every guide that matches the files or behavior being changed. Cross-cutting
changes commonly require more than one guide.

- C++: [.agents/cpp.md](.agents/cpp.md)
- Python and functional-test Python: [.agents/python.md](.agents/python.md)
- Tests, fuzzing, and benchmarks: [.agents/testing.md](.agents/testing.md)
- CMake and compilation: [.agents/build-system.md](.agents/build-system.md)
- Continuous integration: [.agents/ci.md](.agents/ci.md)
- Documentation and release notes: [.agents/documentation.md](.agents/documentation.md)
- Commits, pull requests, and review: [.agents/patches-and-review.md](.agents/patches-and-review.md)
- Consensus and validation: [.agents/consensus-validation.md](.agents/consensus-validation.md)
- `libbitcoinkernel`: [.agents/kernel.md](.agents/kernel.md)
- P2P and networking: [.agents/p2p-networking.md](.agents/p2p-networking.md)
- Mempool and relay policy: [.agents/mempool-policy.md](.agents/mempool-policy.md)
- Storage, serialization, and databases: [.agents/storage-databases.md](.agents/storage-databases.md)
- Wallet: [.agents/wallet.md](.agents/wallet.md)
- RPC and public interfaces: [.agents/rpc-interfaces.md](.agents/rpc-interfaces.md)
- Mining and block assembly: [.agents/mining.md](.agents/mining.md)
- Qt GUI: [.agents/gui.md](.agents/gui.md)

## Verification

Use the narrowest relevant build and test commands, then expand coverage for
consensus, interface, storage, or concurrency changes. Report what was actually
run, including manual checks and known limitations. Never treat an unreviewed
comment, generated summary, or passing narrow check as a substitute for reading
the affected code and canonical documentation.
