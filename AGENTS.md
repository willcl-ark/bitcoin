# AGENTS.md

This repository is used to build and maintain a persistent, LLM-authored wiki
about the Bitcoin Core codebase.

The goal is not one-shot retrieval from raw files. The goal is a durable
markdown knowledge base that compounds over time: subsystem pages, workflow
pages, source summaries, and investigations that are updated as understanding
improves.

## Working Rules

1. Think before writing
- State assumptions explicitly.
- Verify code-path claims in the local checkout before filing them into the
  wiki.
- Prefer the simplest interpretation that matches the sources.

2. The checkout is the primary source of truth
- In this worktree, the Bitcoin Core tree is the raw source corpus.
- Prefer editing `wiki/` and this file.
- Do not edit code under `src/`, `test/`, `doc/`, `contrib/`, `depends/`,
  `ci/`, `cmake/`, or `share/` unless the user explicitly asks to change the
  software rather than the wiki.

3. Make surgical changes
- Touch only the wiki pages needed for the task.
- Avoid broad reorganizations unless the current structure is clearly failing.
- Keep naming and formatting consistent with the existing wiki.

4. Facts first, speculation labeled
- Distinguish verified facts, inferences, and open questions.
- If code and prose disagree, treat the current code as authoritative for
  current behavior and note the discrepancy.
- Never present a hypothesis as settled behavior.

5. Bitcoin Core is security-critical
- Be precise about boundaries between consensus, policy, networking, wallet,
  RPC, GUI, build, and test behavior.
- Do not call something "consensus-critical" unless the sources justify it.
- When concurrency or locking matters, name the relevant locks or thread
  ownership explicitly.

6. Keep durable outputs
- If a question produces reusable understanding, file it back into the wiki.
- Queries, comparisons, code-path explanations, and architecture notes should
  become pages when they would be useful again.

7. Review before handing off
- Re-read the modified pages for accuracy, duplication, and noise.
- Prefer fewer, clearer pages over a large number of shallow ones.

8. Never perform GitHub write actions
- Do not post or edit comments, reviews, issues, or PRs.
- Draft text for the user if they need something to post externally.

9. Keep commits atomic when asked to commit
- Use small commits.
- Commit messages should explain why the wiki change was made, not just what
  changed.

## Purpose

Use this repository to answer questions like:

- What subsystem owns a behavior?
- Which files and symbols implement a workflow?
- What are the key invariants in a component?
- Where is the boundary between consensus and policy?
- Which tests exercise this path?
- What changed in a PR, commit, or release, and how does that relate to the
  current code?

The wiki should optimize for contributors and reviewers who need fast,
trustworthy orientation in a large security-sensitive codebase.

## Layers

There are three layers:

1. Raw sources
- The local checkout is the primary source corpus:
  `src/`, `test/`, `doc/`, `contrib/`, `depends/`, `ci/`, `cmake/`, `share/`,
  and git history when explicitly consulted.
- External sources are secondary and must be labeled as such:
  BIPs, PR discussions, Review Club logs, mailing list posts, release notes,
  and external articles.

2. The wiki
- The wiki lives under `wiki/`.
- It is a derived artifact written and maintained by the agent.

3. The schema
- This file defines how the wiki is structured and maintained.

## Source Priority

Use sources in this order:

1. Current code in the local checkout
2. Local docs and tests in the checkout
3. Git history for intent or evolution
4. External specifications or discussions

Rules:

- Verify behavior in code before treating a PR, commit message, or external
  discussion as authoritative.
- If an external source describes intended behavior that does not match the
  current tree, record both and mark the mismatch clearly.
- When a claim depends on build flags, runtime args, chain selection, wallet
  enablement, or version context, say so.

## Scope Map

The wiki should organize Bitcoin Core around stable architectural areas rather
than around every directory entry.

High-value areas include:

- Consensus and script:
  `src/consensus/`, `src/script/`, `src/primitives/`
- Validation and chainstate:
  `src/validation*`, `src/kernel/`, `src/node/`
- Mempool and policy:
  `src/policy/`, `src/txmempool*`, package acceptance paths
- P2P and networking:
  `src/net*`, `src/net_processing*`, `src/addrman*`, `src/banman*`,
  `src/txrequest*`, `src/node/txdownloadman*`
- Mining and block assembly:
  `src/node/miner.*`, mining RPCs, block template logic
- Wallet:
  `src/wallet/`, wallet RPC, wallet tests
- RPC, REST, ZMQ, interfaces:
  `src/rpc/`, `src/rest.cpp`, `src/rest.h`, `src/zmq/`, `src/interfaces/`
- Common utilities and configuration:
  `src/common/`, `src/util/`
- GUI:
  `src/qt/`
- Testing:
  `src/test/`, `src/wallet/test/`, `src/test/fuzz/`,
  `test/functional/`, `test/lint/`
- Build, packaging, and CI:
  `CMakeLists.txt`, `cmake/`, `depends/`, `ci/`, `contrib/`, `share/`
- Libraries and vendored components:
  `src/secp256k1/`, `src/univalue/`, `src/leveldb/`, `src/minisketch/`,
  `src/crc32c/`

Bias toward subsystem pages and end-to-end workflows. Add file-level pages only
for especially important files or when a question requires that granularity.

## Wiki Layout

Use this layout:

```text
wiki/
  index.md
  log.md
  overview.md
  areas/
  concepts/
  workflows/
  files/
  sources/
  investigations/
```

Definitions:

- `overview.md`
  A high-level map of the codebase and how the major areas fit together.
- `index.md`
  The catalog of wiki pages with one-line summaries.
- `log.md`
  An append-only chronological record of ingests, queries, and lint passes.
- `areas/`
  Stable subsystem pages.
- `concepts/`
  Cross-cutting ideas such as chainstate, mempool policy, package relay,
  descriptors, assumeutxo, fee estimation, addrman, or script verification.
- `workflows/`
  End-to-end flows such as transaction acceptance, block validation and
  connection, IBD, wallet rescan, block relay, or RPC request handling.
- `files/`
  File-level deep dives when needed. Mirror repo paths and append `.md`.
  Example: `src/validation.cpp` becomes `wiki/files/src/validation.cpp.md`.
- `sources/`
  Summaries of ingested sources such as commits, PRs, BIPs, docs, or external
  discussions.
- `investigations/`
  Durable answers produced while exploring a question.

Start simple. Do not create every directory page upfront just because it could
exist.

## Suggested Initial Pages

Bootstrap the wiki with a small set of high-leverage pages:

- `wiki/overview.md`
- `wiki/areas/consensus-and-script.md`
- `wiki/areas/validation-and-chainstate.md`
- `wiki/areas/mempool-and-policy.md`
- `wiki/areas/p2p-and-networking.md`
- `wiki/areas/wallet.md`
- `wiki/areas/rpc-rest-zmq-and-interfaces.md`
- `wiki/areas/common-utils-and-configuration.md`
- `wiki/areas/testing.md`
- `wiki/areas/build-packaging-and-ci.md`
- `wiki/areas/libbitcoinkernel-and-libraries.md`
- `wiki/workflows/transaction-acceptance.md`
- `wiki/workflows/block-validation-and-connection.md`
- `wiki/workflows/initial-block-download.md`

Create more pages only when a source or question justifies them.

## Naming

- Use lowercase kebab-case for area, concept, workflow, source, and
  investigation pages.
- Keep page names stable and descriptive.
- Prefer domain names over directory names when that improves readability.
  Example: use `mempool-and-policy.md` instead of `policy.md` if both are
  covered together.
- File pages should be deterministic:
  `wiki/files/<repo-path>.md`

## Page Format

Use minimal YAML frontmatter when it adds value:

```yaml
---
kind: area
title: Validation and Chainstate
status: active
last_reviewed: 2026-04-20
paths:
  - src/validation.cpp
  - src/validation.h
  - src/node/
tags:
  - validation
  - chainstate
---
```

Preferred sections for area, concept, and workflow pages:

1. Summary
2. Responsibilities or invariants
3. Important code paths
4. Related tests
5. Adjacent pages
6. Open questions
7. Sources consulted

Preferred sections for file pages:

1. Role in the system
2. Important types and functions
3. Callers and dependencies
4. Related tests
5. Notes or risks
6. Sources consulted

Preferred sections for source pages:

1. Source metadata
2. Summary
3. Facts extracted
4. Wiki pages updated
5. Open questions

Do not force empty sections. Omit sections that add no value.

## Citation Rules

Every durable technical claim should be traceable.

Use these rules:

- For current behavior, cite repo paths and symbol names.
- Prefer durable citations such as
  `` `src/validation.cpp` (`ChainstateManager::ProcessNewBlock`) ``.
- Add line numbers only when they materially disambiguate and you are willing
  to update them during lint passes.
- For external sources, include the source type and a link or identifier.
- Use wiki links between pages.

Do not rely on a commit message, PR title, or memory as the only citation for a
behavioral claim.

## Content Rules

When writing Bitcoin Core pages:

- Separate consensus rules from policy rules.
- Separate current behavior from historical motivation.
- Note configuration gates such as command-line args, chain params, wallet
  enablement, and compile-time options.
- Call out important locks, caches, ownership boundaries, and thread
  expectations when relevant.
- Record the tests that cover the behavior:
  unit, functional, fuzz, GUI, or lint.
- Link outward to the most relevant area, concept, workflow, and file pages.
- Prefer summarizing behavior and invariants over copying code.

For vendored or library subtrees, focus on why they exist, their exposed
interfaces, and where Bitcoin Core depends on them. Do not mirror their full
internal documentation unless a task specifically requires it.

## Index Rules

`wiki/index.md` is the navigation entry point.

It should:

- List every durable page with a one-line summary
- Group pages by section:
  overview, areas, concepts, workflows, files, sources, investigations
- Stay concise enough to scan quickly
- Optionally include a short repo snapshot section with branch and `HEAD` when a
  large wiki update or lint pass is completed

Example entry format:

- `[[areas/validation-and-chainstate]]` - Chainstate ownership, block
  processing, activation, and UTXO-related state transitions.

## Log Rules

`wiki/log.md` is append-only and chronological.

Each entry must start with:

```text
## [YYYY-MM-DD] <type> | <subject>
```

Valid types include:

- `ingest`
- `query`
- `lint`
- `restructure`

Each log entry should briefly record:

- What was read
- What pages were created or updated
- What new questions or gaps were found

Keep entries short and factual.

## Operations

### Bootstrap

When starting the wiki or entering a new worktree:

1. Read `README.md`, `CONTRIBUTING.md`, `doc/developer-notes.md`,
   `src/test/README.md`, and `test/README.md`.
2. Scan the top-level repo layout.
3. Create or update `wiki/overview.md`, `wiki/index.md`, and `wiki/log.md`.
4. Create a small set of area pages before going narrower.

### Ingest

Use ingest when adding new source material such as a file, directory, commit,
PR, BIP, doc, or external discussion.

Workflow:

1. Classify the source
- Code file or subtree
- Local documentation
- Commit or PR
- External spec or discussion

2. Read the primary source directly
- Do not summarize from memory or from a secondary source.

3. Extract durable knowledge
- What behavior exists
- What invariants matter
- What code paths and tests are relevant
- What pages this source should update

4. Update the wiki
- Create or update source summaries under `wiki/sources/` when the source has
  standalone value
- Update the relevant area, concept, workflow, and file pages
- Update `wiki/index.md`
- Append an entry to `wiki/log.md`

5. Record unresolved gaps
- Missing tests
- Unclear ownership
- Conflicting docs
- Questions worth future investigation

### Query

When answering a question:

1. Read `wiki/index.md` first
2. Read the most relevant wiki pages
3. Re-verify important claims in the current source tree
4. Answer with citations
5. If the answer is durable, file it under `wiki/investigations/` and link it
   from the relevant pages

Never let the wiki substitute for checking the code on high-risk questions.

### Lint

Run a wiki health check periodically.

Look for:

- Orphan pages with no meaningful inbound links
- Pages missing from `wiki/index.md`
- Stale claims that no longer match the current checkout
- Paths or symbols that no longer exist
- Broken or weak cross-references
- Important concepts repeatedly mentioned but lacking their own page
- Pages that discuss behavior without naming code paths or tests
- Contradictions between area, workflow, and source pages
- Historical or external claims that are not clearly labeled as such

Favor small corrective edits over large rewrites.

## Review and Analysis Guidance

For Bitcoin Core review-oriented work, the wiki should be especially good at:

- mapping changed files into subsystems
- surfacing consensus, policy, and wallet boundaries
- naming locks and shared-state ownership
- identifying affected tests
- linking a PR or commit to the current implementation

If asked to review code, use the wiki as context, but inspect the actual diff
and source directly before reaching conclusions.

## Non-Goals

The wiki is not:

- a generated reference for every file in the repo
- a substitute for reading code on risky questions
- a place for ungrounded speculation
- a copy of external documentation when a tighter synthesis would do

Prefer a small number of accurate, connected pages over exhaustive but shallow
coverage.
