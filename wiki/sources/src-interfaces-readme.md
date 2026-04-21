---
kind: source
title: src/interfaces/ README
status: active
last_reviewed: 2026-04-21
source_path: src/interfaces/README.md
tags:
  - source
  - interfaces
  - multiprocess
---

# src/interfaces/ README

## Source metadata

- Source type: local documentation
- Path: `src/interfaces/README.md`
- Scope: internal C++ interface boundaries between node, wallet, GUI, RPC, and IPC code

## Summary

`src/interfaces/README.md` documents the internal interface types used to keep
node, wallet, GUI, RPC, and multiprocess code loosely coupled. It is primarily
an ownership and architecture note: these interfaces make major components
easier to test, evolve, and potentially run in different processes, but they
are not promised as stable external APIs.

## Facts extracted

- `Chain` is the wallet-facing view onto blockchain and mempool state.
- `ChainClient` is the node-side lifecycle hook for starting and stopping
  chain clients.
- `Node` and `Wallet` are the main GUI-facing interfaces.
- `Handler` objects manage event-handler lifetimes returned by interface
  subscriptions.
- `Init` and `Ipc` support multiprocess startup and cross-process interface
  access.
- `Rpc` exists so `bitcoin-cli` can call RPC methods over a Unix socket instead
  of TCP.
- The document explicitly says these interfaces are not currently designed to
  be stable or externally consumed.

## Wiki pages updated

- `[[overview]]`
- `[[index]]`

## Open questions

- The README names the major interface classes, but deeper ownership details
  still need file-level or workflow-level wiki pages for callback threading,
  lifetime rules, and shutdown ordering.

