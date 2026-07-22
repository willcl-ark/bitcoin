# Consensus and validation

Operational rules for modifying Bitcoin Core's consensus and validation code. Consensus bugs can cause chain splits; these rules reflect project-wide invariants.

## Process and scope

- **Network-wide consensus changes** must first be discussed extensively on the [bitcoindev mailing list](https://groups.google.com/g/bitcoindev), accompanied by a widely discussed BIP, and supported by broad technical consensus. Complicated or controversial P2P protocol changes should also be discussed publicly before implementation.
- **Consensus changes** require extensive public discussion, broad technical consensus, and a higher review bar. Consensus-critical refactors demand exceptional caution.
- Treat **storage-library changes** as potential consensus-compatibility changes when returned data can differ.

## Code boundaries

- **Do not add policy code to consensus-critical files** (e.g., `src/script/script.h`). Pure policy code belongs in the `src/policy/` directory. Mixing domains risks accidental consensus changes and chain splits.
- **Core consensus and validation code must not contain network-specific knowledge** (genesis blocks, network names). Use chain parameters to control network-specific rules.
- **Do not place test-only code in production source files**, especially in consensus-critical files. This prevents proper testing and review in production environments.

## Validation invariants

- Never use **assertion/check helpers** (`assert`, `Check`, `Assert`) to validate network, user, or disk input.
- **Do not use invariant-enforcing types** (e.g., `not_null`, `safe_signed_range`) in consensus validation code. Block validation must distinguish between *valid*, *invalid*, and *runtime error*. Invalid blocks must be representable without runtime failures, and runtime errors must not be caught and interpreted as invalidity.
- **Headers are append-only and never deleted**, making them a significant memory DoS vector. Keep explicit proof-of-work checks on headers as a defensive measure even if the argument is currently always true.
- **Fields from untrusted file metadata** (e.g., block height in UTXO snapshots) must be validated before use or, if redundant with trusted data derivable from another field, removed.

## Flags, deployments, and testing

- **When adding new consensus script verification flags**, update `MANDATORY_SCRIPT_VERIFY_FLAGS` accordingly. Consider compile-time assertions (`static_assert`) to prevent silent divergence between consensus flags and mandatory policy flags.
- **Tests for consensus-sensitive behavior** must not depend on configurable default values; explicitly test both possible outcomes to remain correct when defaults change.
- **When modifying consensus-critical structs or fields**, rename them so that unmodified references become obvious in diffs and future contributors cannot accidentally assume old semantics.
- **Prefer synthetic tests** (unit, fuzz, or functional) that provide full coverage of consensus rules over reliance on real-world data. Shortcomings found in real data should be added as synthetic tests.

See [`src/consensus/`](../src/consensus/) and [`doc/developer-notes.md`](../doc/developer-notes.md) for canonical reference material.
