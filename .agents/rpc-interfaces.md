# RPC and interfaces

This guide covers Bitcoin Core conventions for RPC and command-line interface design, deprecation, and testing.

## General principles

- **Expose via RPC first.** New features should ship through RPC before the GUI where appropriate.
- **Keep adapters thin.** Interface code delegates substantial logic to the owning component. The wallet layer must not depend on RPC code (no cyclic dependencies).
- **Use CHECK_NONFATAL** over `assert()` in RPC and interface code for friendlier error handling.

## RPC response design

- **Return JSON objects**, not bare arrays or scalar values, so fields can be added later without breakage.
- **Use JSON arrays** (not JSON strings) to represent list-like data.
- **Prefer underscores** over hyphens in JSON field names for language compatibility.
- **Avoid spaces** in JSON keys; they cause interoperability problems for consumers.
- **Prefer optional/omitted fields** over magic sentinel values (e.g. `0` or `-1`) when a field has no meaningful value.
- **Do not introduce unnecessary sorting** in RPC output — sorting creates implicit API guarantees that are hard to remove later.
- **For filesystem paths in responses**, use `fs::path::u8string()` or `fs::path::utf8string()`, not `PathToString` (JSON requires UTF-8 per RFC 8259).

## Error handling

- **Validate input parameters** and return descriptive error messages listing acceptable values.
- **Use the correct RPC error code:** `RPC_INTERNAL_ERROR` only for genuine bitcoind errors (e.g. datadir corruption); use `RPC_DESERIALIZATION_ERROR` or `RPC_MISC_ERROR` for input issues.
- **Include explanatory messages** when adding new limitations or surprising behavior.
- **Do not duplicate RPC error responses** in separate logs — the RPC response is the proper channel.
- **Do not rely on matching error strings** in tests or downstream software. Error codes are the stable contract.

## Deprecation

- **Maintain backward compatibility** through the documented `-deprecatedrpc` cycle. See [doc/json-rpc.md](../doc/json-rpc.md) for the standard deprecation mechanism.
- **Add deprecation tests** in `test/functional/rpc_deprecated.py`.
- **Mark deprecated fields** as such in RPC documentation to signal eventual removal.

## CLI and documentation

- **For repeatable options**, follow last-one-wins semantics.
- **Use named arguments** (not positional) when calling RPCs with boolean parameters.
- **Keep RPC help and documentation stable** — avoid brittle, mutable, or user-specific information (e.g. home directories).
- **Update documentation** (help texts, RPC descriptions) whenever CLI or RPC behavior changes.
- **Do not add test-only functionality** to production RPC interfaces.

For the canonical RPC conventions, refer to [doc/developer-notes.md](../doc/developer-notes.md) and [doc/json-rpc.md](../doc/json-rpc.md).
