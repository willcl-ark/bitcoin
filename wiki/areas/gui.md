---
kind: area
title: GUI
status: active
last_reviewed: 2026-04-21
paths:
  - src/qt/
  - src/init/bitcoin-qt.cpp
  - src/interfaces/node.h
  - src/interfaces/wallet.h
tags:
  - gui
  - qt
  - wallet
---

# GUI

## Summary

The GUI area is the Qt front-end for Bitcoin Core. It owns application startup,
main-window composition, wallet views, diagnostic consoles, user-configurable
options, and payment-request handling. The GUI talks to node and wallet code
through `interfaces::Node` and `interfaces::Wallet`, rather than reaching
directly into validation or wallet internals.

## Responsibilities and Invariants

- `src/init/bitcoin-qt.cpp` and `src/qt/bitcoin.cpp` start the Qt application,
  create the node interface, and wire `BitcoinApplication`, `BitcoinGUI`,
  `ClientModel`, and (when enabled) `WalletController`.
- `src/qt/bitcoingui.cpp` (`BitcoinGUI`) is the main window and high-level UI
  coordinator. It owns the wallet selector, tray/menu actions, status bar,
  modal overlay, and RPC console integration.
- `src/qt/clientmodel.*` is the node-facing model for GUI code. It surfaces
  network state, block-tip state, warnings, peers, and options via
  `interfaces::Node`.
- Wallet UI composition is split across:
  - `src/qt/walletcontroller.cpp` (`WalletController`) for loaded-wallet
    lifetime and activities such as create/open/restore/migrate.
  - `src/qt/walletframe.cpp` (`WalletFrame`) for multiwallet frame management.
  - `src/qt/walletview.cpp` (`WalletView`) for the per-wallet stacked UI.
  - `src/qt/walletmodel.*` for the wallet-facing Qt model backed by
    `interfaces::Wallet`.
- The node and wallet windows stay separated at the interface boundary:
  `BitcoinGUI` and `RPCConsole` depend on `interfaces::Node`, while wallet tabs
  and dialogs depend on `WalletModel` / `interfaces::Wallet`.
- The GUI is optional at build time. `CMakeLists.txt` gates it with
  `BUILD_GUI`, and wallet-enabled GUI features depend on `ENABLE_WALLET`.

## Important Code Paths

- Application startup:
  `src/init/bitcoin-qt.cpp`, `src/qt/bitcoin.cpp`
- Main window and node UI:
  `src/qt/bitcoingui.cpp`, `src/qt/clientmodel.cpp`,
  `src/qt/rpcconsole.cpp`, `src/qt/optionsmodel.cpp`
- Wallet integration:
  `src/qt/walletcontroller.cpp`, `src/qt/walletframe.cpp`,
  `src/qt/walletview.cpp`, `src/qt/walletmodel.cpp`
- Initialization thread handoff:
  `src/qt/initexecutor.cpp`
- Payment-request handling:
  `src/qt/paymentserver.cpp`

## Related Tests

- GUI test entry point: `src/qt/test/test_main.cpp`
- GUI tests: `src/qt/test/apptests.cpp`, `src/qt/test/wallettests.cpp`,
  `src/qt/test/addressbooktests.cpp`, `src/qt/test/optiontests.cpp`,
  `src/qt/test/rpcnestedtests.cpp`

## Adjacent Pages

- `[[areas/wallet]]`
- `[[areas/rpc-rest-zmq-and-interfaces]]`
- `[[areas/common-utils-and-configuration]]`
- `[[areas/testing]]`

## Sources Consulted

- `src/qt/README.md`
- `src/init/bitcoin-qt.cpp`
- `src/qt/bitcoin.cpp`
- `src/qt/bitcoingui.cpp`
- `src/qt/clientmodel.cpp`
- `src/qt/initexecutor.cpp`
- `src/qt/optionsmodel.cpp`
- `src/qt/paymentserver.cpp`
- `src/qt/rpcconsole.cpp`
- `src/qt/walletcontroller.cpp`
- `src/qt/walletframe.cpp`
- `src/qt/walletmodel.cpp`
- `src/qt/walletview.cpp`
- `src/qt/test/test_main.cpp`
- `src/qt/CMakeLists.txt`
- `CMakeLists.txt`
