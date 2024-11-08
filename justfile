set shell := ["bash", "-uc"]

os := os()

default:
    just --list

# Build default project
[group('build')]
build *args: clean
    cmake -B build {{ args }}
    cmake --build build -j {{ num_cpus() }}

# Build with all optional modules
[group('build')]
build-dev *args: clean
    cmake -B build --preset dev-mode {{ args }}
    cmake --build build -j {{ num_cpus() }}

# Build for the CI, including bench_bitcoin
[private]
[group('ci')]
build-ci: clean
    cmake -B build -DBUILD_BENCH=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo -DAPPEND_CPPFLAGS="-fno-omit-frame-pointer"
    cmake --build build -j {{ num_cpus() }}

# Re-build current config
[group('build')]
rebuild:
    cmake --build build -j {{ num_cpus() }}

# Clean build dir using git clean -dfx
[group('build')]
clean:
    git clean -dfx

# Run unit tests
[group('test')]
test-unit:
    ctest --test-dir build -j {{ num_cpus() }}

# Run all functional tests
[group('test')]
test-func:
    build/test/functional/test_runner.py -j {{ num_cpus() }}

# Run all unit and functional tests
[group('test')]
test: test-unit test-func

# Run a single functional test (filename.py)
[group('test')]
test-func1 test:
    build/test/functional/test_runner.py {{ test }}

# Run a single unit test suite
[group('test')]
test-unit1 suite:
    build/src/test/test_bitcoin --log_level=all --run_test={{ suite }}

# Run benchmarks
[group('perf')]
bench:
    build/src/bench/bench_bitcoin

# Run the lint job
lint:
    #!/usr/bin/env bash
    cd test/lint/test_runner/
    cargo fmt
    cargo clippy
    export COMMIT_RANGE="$( git rev-list --max-count=1 --merges HEAD )..HEAD"
    RUST_BACKTRACE=1 cargo run

# Run the CI workflow
[group('ci')]
run-ci: build-ci bench test

[private]
[confirm("Are you sure you want to delete everything in TMP_DATADIR?")]
_confirm-clean-datadir TMP_DATADIR:
    rm -Rf {{TMP_DATADIR}}/*

[private]
_clean-datadir TMP_DATADIR:
    rm -Rf {{TMP_DATADIR}}/*

# Check we aren't about to nuke a live datadir
[private]
check-datadir TMP_DATADIR:
    #!/usr/bin/env bash

    # Create the directory
    mkdir -p {{TMP_DATADIR}}

    # Check if the "CI" env var is set
    if [ -n "$CI" ]; then
        # If it is, call _clean-datadir
        just _clean-datadir {{TMP_DATADIR}}
    else
        # If not, call _confirm-clean-datadir
        just _confirm-clean-datadir {{TMP_DATADIR}}
    fi

[private]
prepare-assumeutxo-snapshot-run commit TMP_DATADIR UTXO_PATH:
    git checkout {{commit}}
    just build-ci
    just check-datadir {{TMP_DATADIR}}
    build/src/bitcoind -datadir={{TMP_DATADIR}} -connect=148.251.128.115:55555 -daemon=0 -signet -stopatheight=1
    -build/src/bitcoind -datadir={{TMP_DATADIR}} -connect=148.251.128.115:55555 -daemon=0 -signet -dbcache=16000 -pausebackgroundsync=1 -loadutxosnapshot={{UTXO_PATH}}

# Run assumeutxo CI workflow
[group('ci')]
run-assumeutxo-signet-ci $TMP_DATADIR $UTXO_PATH base_commit head_commit results_dir:
    hyper-wrapper ./hyperfine-config.json {{base_commit}} {{head_commit}} {{results_dir}}
