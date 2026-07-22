# Documentation

This guide covers documentation conventions for Bitcoin Core. See [`../CONTRIBUTING.md`](../CONTRIBUTING.md) for contribution process, [`../doc/developer-notes.md`](../doc/developer-notes.md) for developer conventions, and [`../doc/release-notes-empty-template.md`](../doc/release-notes-empty-template.md) for release note structure.

## Code Comments

- **Document public API entities** with Doxygen-compatible comments where the
  developer notes require them, covering rationale and parameter/member meaning
  without duplicating obvious type information.
- **Explain *why*, not *what*** — document rationale, design intent, and non-obvious invariants rather than restating the code.
- **Keep comments concise** and omit comments on self-explanatory code; unnecessary comments add maintenance burden and risk staleness.
- **Use `//!` only for the immediately following declaration.** For broader context, use plain comments (`//` or `/* */`).
- **Document provenance** for non-obvious constants, magic numbers, and cryptographic values — explain origin, rationale, and security assumptions.
- **Add inline comments** for temporary workarounds, compatibility shims, and platform-specific hacks, stating why they exist and the removal conditions.
- **Use the issue tracker** to track outstanding work instead of TODO/FIXME comments; issues capture context and discussion history.

## Documentation Files

- **Update docs with behavior changes.** When modifying code, update all affected documentation files, RPC help texts, and configuration option descriptions.
- **Describe current behavior**, not desired or intended behavior. Propose behavioral changes in a separate PR that also updates docs.
- **Prefer links over duplication** — link to canonical `doc/` files or authoritative external docs rather than copying content inline.
- **Use consistent Markdown style:** `#` headers, 80–100 character line wrapping, blank lines after headers, relative links, no manual section numbering.
- **Follow terminology conventions:** correct spelling, proper nouns capitalized (e.g., "Windows"), correct possessives ("its" not "it's"), consistent Bitcoin terms ("coinbase transaction", "change addresses").
- **Avoid embedding version numbers or dates** in build/dependency docs; use generic descriptions that stay accurate over time.

## Release Notes

- **Add PR-specific release notes** for: new features, behavior changes, removed features, default value changes, RPC/API compatibility breaks, lifted restrictions, and any user- or operator-visible change.
- **Write for end users** in present tense, following section names from `../doc/release-notes-empty-template.md`.
- **Do not modify historical/archived** release note files, even if they contain minor issues.

## Logging

- Use project logging levels as defined in `../doc/developer-notes.md`: `LogError`/`LogFatal` for abort conditions, `LogWarning` for operator-attention issues, `LogInfo` for state changes, `LogDebug`/`LogTrace` (with BCLog category) for verbose debug output.
