# Functional test profiling

Set `BITCOIN_TEST_PROFILE=1` when running `test/functional/test_runner.py` to
write one JSON profile per test. The runner writes profiles to
`BITCOIN_TEST_PROFILE_DIR` when it is set, or to `BUILDDIR/test-profiles` by
default. The GitHub Actions CI workflow enables this on this branch and uploads
the JSON files as `test-profiles-*` artifacts.
Each runner invocation writes into a unique `run-*` subdirectory so repeated
local runs do not mix profile data.

Each per-test JSON file has version `1`, metadata for the invocation, and a
`timings` array:

```json
{
  "version": 1,
  "metadata": {
    "argv": ["..."],
    "randomseed": 123,
    "timeout_factor": 40
  },
  "timings": [
    {
      "name": "test_method.ExampleTest.run_test",
      "calls": 1,
      "seconds": 3.2,
      "min_seconds": 3.2,
      "max_seconds": 3.2
    }
  ]
}
```

`summary.json` in the same directory contains the runner result table and
timings aggregated across all completed tests. Timings are inclusive, so nested
sections are useful for ranking work but should not be added together as a
complete breakdown of runtime.

For a local run:

```bash
BITCOIN_TEST_PROFILE=1 \
BITCOIN_TEST_PROFILE_DIR=/tmp/bitcoin-functional-profiles \
build/test/functional/test_runner.py -j8 --filter 'wallet_|mempool_'
```

The existing `--test_methods` option remains useful for running a specific
independent method inside a large functional test after the profile data points
to a slow section.

Use `--profiledir=/tmp/bitcoin-functional-profiles` to enable profiling and
choose the destination in one option. A direct test invocation can instead set
`BITCOIN_TEST_PROFILE=1` and `BITCOIN_TEST_PROFILE_FILE=/tmp/test.json`.

The profiles include scenario methods, named heavy phases, RPC calls, node
startup and teardown, CLI subprocesses, MiniWallet rescans, and framework
unittest cases. Poll counters and scheduled sleep totals help distinguish
polling delays from useful work. Passed, skipped and failed tests retain their
profiles outside the test directories cleaned by the framework. Cache creation
has its own profile. Processes killed before cleanup may have no profile.

Benchmark sanity checks write a sibling `.bench.json` file. Its `seconds`
includes the entire benchmark function; `nanobench_seconds` sums all retained
nanobench timed results. Their difference includes fixture construction,
teardown, warmup and measurement overhead. Normal benchmark workloads remain
unchanged. Compare profiles from the same CI job and test variant; these are
wall times under concurrent load, not isolated CPU benchmarks.
