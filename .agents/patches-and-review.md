# Patches and review

This guide covers the conventions and expectations for submitting, formatting, and reviewing pull requests in Bitcoin Core.

## Commit structure

- **Keep commits atomic.** Each commit must compile and pass CI independently, supporting `git bisect` and clean intermediate states.
- **One logical change per commit.** Do not mix bugfixes, features, refactors, style changes, moves, or renames in the same commit.
- **Separate moves and renames** into dedicated move-only commits so that review tools (`--color-moved=dimmed-zebra`) work effectively.
- **Do not introduce-and-fix across commits.** Squash together changes that modify a line then revert or rework it in a later commit within the same PR.
- **Refactoring commits must not change behavior.** Put behavioral changes in separate commits or PRs.

## Commit messages

- Write in the **imperative mood** with a short summary line (~50 characters), followed by a blank line and body wrapped at ~72 characters.
- Explain **why** (rationale), not just **what**. See [../CONTRIBUTING.md](../CONTRIBUTING.md) for details.
- Use **recognized conventional prefixes** (e.g., `test:`, `refactor:`, `build:`, `doc:`, `rpc:`).
- Do **not** include `@`-mentions, as these leak into merge commit messages and cause unwanted notifications on cherry-picks.
- Backport commits must include `Github-Pull: #num` and `Rebased-From: <commit>` metadata.

## Pull requests

- Write the PR description as **standalone prose** explaining motivation, goals, and trade-offs — not a list of commits or repetition of the diff.
- **Do not rebase unnecessarily** after ACKs; rebasing invalidates ACKs without benefit unless there is an actual conflict.
- **Do not request review when CI is failing.** Mark the PR as draft if it is not ready.
- **Do not open PRs for trivial typos or whitespace-only fixes.** Make such changes organically when touching code for other reasons.
- **Do not duplicate existing open PRs.** Check for prior work before submitting.
- User-visible changes require a **release note**. See [../doc/release-notes.md](../doc/release-notes.md).

## Review process

- **The author is responsible** for addressing feedback, explaining changes, defending design decisions, and ensuring quality before requesting review.
- **Scripted-diff commits** for mechanical bulk changes (renames, refactors across many files) must use a `BEGIN VERIFY SCRIPT` / `END VERIFY SCRIPT` block for reproducibility.
- **Avoid purely stylistic changes** to code not being modified functionally. Style not documented in [../doc/developer-notes.md](../doc/developer-notes.md) is left to the author's preference.
- **Upstream subtrees first:** Submit changes to vendored code (leveldb, secp256k1, minisketch, etc.) upstream, not as direct PRs against Bitcoin Core.
- ACK/NACK conventions follow [../CONTRIBUTING.md](../CONTRIBUTING.md).

## Verification

- Use `git range-diff` to verify rebases and force-pushes. Review your own diff on GitHub before requesting review.
- Ensure every intermediate commit compiles and passes CI ([../CONTRIBUTING.md](../CONTRIBUTING.md)).
