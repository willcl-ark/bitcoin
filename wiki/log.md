# Wiki Log

## [2026-04-20] ingest | bootstrap overview, areas, and workflows

- Read `AGENTS.md`, `README.md`, `CONTRIBUTING.md`, `doc/developer-notes.md`, `doc/README.md`, `src/node/README.md`, `src/interfaces/README.md`, `src/test/README.md`, `test/README.md`, `CMakeLists.txt`, and targeted subsystem files.
- Created `wiki/overview.md`, `wiki/index.md`, ten area pages, and three workflow pages covering the highest-value Bitcoin Core subsystems and end-to-end paths.
- Gaps noted: no concept pages, file pages, source summaries, or investigation pages yet; mining, GUI, and narrower file-level deep dives should be added when specific questions justify them.

## [2026-04-21] ingest | expand areas, concepts, file pages, and RPC workflow

- Read targeted code and docs for mining, GUI, chainstate/assumeutxo, descriptors, script verification, addrman, fee estimation, `src/validation.cpp`, and HTTP RPC dispatch.
- Created `wiki/areas/mining-and-block-assembly.md`, `wiki/areas/gui.md`, six concept pages, `wiki/files/src/validation.cpp.md`, and `wiki/workflows/rpc-request-handling.md`.
- Updated `wiki/overview.md` and `wiki/index.md` to include the new pages and cross-cutting concept layer.

## [2026-04-21] ingest | critical-path file pages and priority map

- Appended a critical-priority rubric to `AGENTS.md` covering crash, offline, OOM/resource, fund-loss, operator-privacy, and sender/receiver-privacy risks.
- Created file pages for `src/net.cpp`, `src/net_processing.cpp`, `src/txmempool.cpp`, `src/wallet/spend.cpp`, and `src/httprpc.cpp`, plus `wiki/investigations/critical-codepaths-priority-map.md`.
- Updated overview, index, and adjacent links so critical-path review can start from one investigation page and drill into the corresponding files.

## [2026-04-21] ingest | critical concepts and wallet-miner deep dives

- Read targeted wallet, mining, networking, HTTP, orphanage, txdownload, pruning, and RPC files to turn the critical rubric into reusable concept pages.
- Created `wiki/concepts/operator-privacy.md`, `wiki/concepts/transaction-sender-and-receiver-privacy.md`, `wiki/concepts/resource-exhaustion-and-backpressure.md`, `wiki/concepts/wallet-fund-safety.md`, `wiki/files/src/wallet/wallet.cpp.md`, and `wiki/files/src/node/miner.cpp.md`.
- Updated overview, index, the critical priority map, and nearby area/workflow links so privacy, availability, and fund-safety review surfaces are reachable from the main wiki entry points.

## [2026-04-21] ingest | source summaries and startup-relay-wallet workflows

- Read targeted code and docs for init/shutdown, block relay, wallet rescans, package policy, tx download scheduling, and the repo's local architecture/testing documents.
- Created `wiki/workflows/node-startup-and-shutdown.md`, `wiki/workflows/block-relay.md`, `wiki/workflows/wallet-rescan.md`, `wiki/concepts/package-policy-and-relay.md`, `wiki/files/src/node/txdownloadman_impl.cpp.md`, and source summary pages for `doc/developer-notes.md`, `src/node/README.md`, `src/interfaces/README.md`, `src/test/README.md`, and `test/README.md`.
- Updated `wiki/overview.md`, `wiki/index.md`, and nearby area/workflow pages so startup, relay, rescans, package policy, tx download internals, and local docs are reachable from the main navigation.

## [2026-04-21] ingest | vulnerability-oriented startup, routing, and coverage pass

- Read targeted code and tests for `src/init.cpp`, `src/addrman.cpp`, `src/node/txorphanage.cpp`, `src/wallet/load.cpp`, `src/httpserver.cpp`, `src/httprpc.cpp`, `src/rpc/server.cpp`, `src/wallet/rpc/util.cpp`, and the test files tied to those paths.
- Created `wiki/files/src/init.cpp.md`, `wiki/files/src/addrman.cpp.md`, `wiki/files/src/node/txorphanage.cpp.md`, `wiki/files/src/wallet/load.cpp.md`, `wiki/concepts/rpc-authentication-and-wallet-routing.md`, and `wiki/investigations/critical-test-coverage-gaps.md`.
- Updated `wiki/overview.md`, `wiki/index.md`, `wiki/investigations/critical-codepaths-priority-map.md`, and nearby area/concept/workflow pages so startup ordering, wallet load boundaries, addrman privacy properties, orphanage anti-DoS behavior, RPC auth/routing, and critical coverage gaps are reachable from the main navigation.

## [2026-04-21] ingest | security concern rollup and action backlog

- Consolidated the current security-relevant findings from the critical priority map, coverage-gap analysis, privacy pages, and file pages into a single concern rollup.
- Created `wiki/investigations/security-review-concerns.md` and `wiki/investigations/actionable-security-work-items.md`.
- Updated `wiki/index.md`, `wiki/overview.md`, and `wiki/investigations/critical-codepaths-priority-map.md` so the concern rollup and work backlog are reachable from the main navigation and critical review entry points.
