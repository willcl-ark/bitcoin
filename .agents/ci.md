# Continuous integration

CI enforces project rules and catches regressions before merge. All CI logic
should be reproducible locally using the same repository scripts.

## Architecture & scripting

- **Extract logic from YAML.** Place CI logic in vendor-agnostic Python or
  shell scripts under [`../ci/`](../ci/) rather than in CI-provider YAML
  files.
- **Prefer Python over Bash.** When Bash is necessary, use `set -o errexit
  -o pipefail -o xtrace` for robust fail-fast behavior.
- **Use long-form options** in shell scripts (e.g. `--tags` over `-t`).
- **Pin external dependencies.** Use specific commits or tags for CI images,
  actions, and tools — never floating branches or `:latest`. Update cache keys
  when versions change.
- **Use LTS images** for toolchains to reduce update churn.
- **Match runner size to task.** Use small instances for single-threaded work
  (linting) and larger ones for parallel compilation. Use
  `strategy.fail-fast: false` so all matrix jobs report results.
- **Limit third-party GitHub Actions.** They risk breakage and supply-chain
  attacks. Enable only when the risk is understood and accepted.

## Linting & static analysis

- **Keep lint jobs clean.** Every warning must be fixed or explicitly
  suppressed. Noisy jobs that pass despite warnings degrade CI value.
- **Enable new clang-tidy checks globally** or not at all. Suppress individual
  false positives with `NOLINT` comments rather than maintaining exception
  lists.
- **Use `git ls-files`** when CI lint scripts enumerate project files, not
  filesystem globs.
- **Show the expected fix diff** when a lint check is auto-fixable, so
  developers can apply the correction without running the tool locally.

## Compilation & toolchain

- **Treat compiler warnings as errors** (`-Werror`) in CI so they surface in
  logs.
- **Keep CI toolchain versions in sync** with the
  [Guix build environment](../contrib/guix/). Cross-compilation jobs must
  match Guix compiler versions.
- **Maintain at least one CI config** using the minimum required compiler
  version declared in [`../doc/dependencies.md`](../doc/dependencies.md).

## Workflow

- **Do not skip CI for doc-only changes.** Most linters apply to all files and
  the savings are negligible.
- **Track known CI failures** via GitHub issues labeled `CI failed`. Add
  category labels (`Test`, `Upstream`, etc.) to clarify the root cause.
- **Avoid linking to ephemeral CI logs** in commit messages or comments — logs
  are cleared after the retention period.
- **Remove old CI config in the same commit** when migrating a task between
  providers.
- **Use `--fail` with `curl`** in CI scripts so HTTP errors stop the pipeline.
- **Place shared cross-distro dependencies** in `CI_BASE_PACKAGES` to avoid
  failures when packages vanish from vanilla container images.

See [`../ci/README.md`](../ci/README.md) for setup and available tasks.
