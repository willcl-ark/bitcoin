set shell := ["bash", "-euc"]
set quiet

[private]
default:
    just --list

# Build Bitcoin Core using the Guix release toolchain
build:
    ./contrib/guix/guix-dev

# Run unit and functional tests
test:
    ctest --test-dir ./build -j {{ num_cpus() }}
    ./build/test/functional/test_runner.py -j {{ num_cpus() }}

# Run the linter
lint:
    ./ci/lint.py
