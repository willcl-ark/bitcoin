---
kind: source
title: src/node/ README
status: active
last_reviewed: 2026-04-21
source_path: src/node/README.md
tags:
  - source
  - node
  - interfaces
---

# src/node/ README

## Source metadata

- Source type: local documentation
- Path: `src/node/README.md`
- Scope: intended ownership boundary for `src/node/` relative to wallet and GUI code

## Summary

`src/node/README.md` defines `src/node/` as the place for code that needs
direct access to shared node state such as chain, block index, coins view, and
mempool state. It also documents a structural boundary: `src/node/`,
`src/wallet/`, and `src/qt/` should avoid direct calls into one another and use
`src/interfaces/` as the narrower coupling layer.

## Facts extracted

- The document treats shared node state as the main criterion for code that
  belongs under `src/node/`.
- It explicitly frames `src/node/` / `src/wallet/` / `src/qt/` separation as a
  safeguard so wallet or GUI changes do not interfere with core node
  operation.
- The same separation is presented as an enabler for multiprocess or
  separately-maintained components, not just as a directory-layout preference.
- The directory is described as intentionally sparse today, with the document
  naming `src/validation.cpp` and `src/txmempool.cpp` as examples of files that
  might move under `src/node/` in the future. That is motivation, not a claim
  about current layout.

## Wiki pages updated

- `[[overview]]`
- `[[index]]`

## Open questions

- The document gives the intended boundary, but not a comprehensive rule for
  which existing files still violate or blur it in the current tree.

