set shell := ["bash", "-uc"]

os := os()

alias rb := rebuild
alias c := check
alias b := build
alias bd := build-dev
alias p := prepare
alias t := test

################################################################################
# This justfile can be used as-is inside the bitcoin core source tree.
#
# However to keep a clean source directory it can also be located in a parent
# directory . e.g.
#
# src/core/
# ├── bitcoin
# │   ├── build
# │   ├── ci
# │   ├── cmake
# │   ├── CMakeLists.txt
# │   ├── CMakePresets.json
# │   ├── contrib
# │   ├── CONTRIBUTING.md
# │   ├── COPYING
# │   ├── depends
# │   ├── doc
# │   ├── INSTALL.md
# │   ├── libbitcoinkernel.pc.in
# │   ├── README.md
# │   ├── SECURITY.md
# │   ├── share
# │   ├── src
# │   ├── test
# │   └── vcpkg.json
# └── justfile
#
# And the directive:
#
# `set working-directory := "bitcoin"`
#
# can be set at the top of this file to indicate the working directory commands
# will be executed in.
#
# Alternatively you could create a new justfile in the parent directory and
# import this justfile into it, allowing you to override certain commands and
# set additional options. This help maintain customisations vs upstream
# changes.
# Set the following directives in the new justfile:
#
# ```
# import '/path/to/this/cloned/justfile'
#
# set dotenv-load := true
# set working-directory := "bitcoin"
# set allow-duplicate-recipes := true
#
# # Override functional test command to include a cachedir and tmpdir
# [group('test')]
# test-func:
#     build/test/functional/test_runner.py -j {{ num_cpus() }} --cachedir=/mnt/tmp/cache --tmpdir=/mnt/tmp
# ```

################################################################################
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
[group('ci')]
build-ci: clean
    cmake -B build -DBUILD_BENCH=ON
    cmake --build build -j {{ num_cpus() }}
# Re-build current config
[group('build')]
rebuild:
    cmake --build build -j {{ num_cpus() }}

[private]
check-build:
    #!/usr/bin/env bash
    set -euxo pipefail

    # If no build, then build it!
    if [ ! -d "build" ]; then
        just build
    fi

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
test: check-build test-unit test-func

# Run a single functional test (filename.py)
[group('test')]
test-func1 test:
    build/test/functional/test_runner.py {{ test }}

# Run a single unit test suite
[group('test')]
test-unit1 suite:
    build/src/test/test_bitcoin --log_level=all --run_test={{ suite }}

# Run clang-format-diff on top commit
[no-exit-message]
[private]
format-commit:
    git diff -U0 HEAD~1.. | ./contrib/devtools/clang-format-diff.py -p1 -i -v

# Run clang-format on the diff (must be configured with clang)
[no-exit-message]
[private]
format-diff:
    git diff | ./contrib/devtools/clang-format-diff.py -p1 -i -v

# Run clang-tidy on top commit
[no-exit-message]
[private]
tidy-commit:
    git diff -U0 HEAD~1.. | ( cd ./src/ && clang-tidy-diff-17.py -p2 -j {{ num_cpus() }} )

# Run clang-tidy on the diff (must be configured with clang)
[no-exit-message]
[private]
tidy-diff:
    git diff | ( cd ./src/ && clang-tidy-diff-17.py -p2 -j $(nproc) )

# Run the linter
[group('lint')]
lint:
    #!/usr/bin/env bash
    set -euxo pipefail
    # uses subshell to load any python venv for flake8

    cd test/lint/test_runner/
    cargo fmt
    cargo clippy
    COMMIT_RANGE="$( git rev-list --max-count=1 --merges HEAD )..HEAD" cargo run

# Run all linters, clang-format and clang-tidy on top commit
[group('lint')]
lint-commit: lint
    just format-commit
    just tidy-commit

# Run all linters, clang-format and clang-tidy on diff
[group('lint')]
lint-diff: lint
    just format-diff
    just tidy-diff

# Lint (top commit), build and test
[group('pr tools')]
check: lint-commit build test-func

# Interactive rebase current branch from (git merge-base) (`just rebase -i` for interactive)
[confirm("Warning, unsaved changes may be lost. Continue?")]
[no-exit-message]
[group('git')]
rebase *args:
    git rebase {{ args }} `git merge-base HEAD upstream/master`

# Update upstream/master and interactive rebase on it (`just rebase-upstream -i` for interactive)
[confirm("Warning, unsaved changes may be lost. Continue?")]
[group('git')]
rebase-upstream *args:
    git fetch upstream
    git rebase {{ args }} `git merge-base HEAD upstream/master`

# Check each commit in the branch passes `just lint`
[confirm("Warning, unsaved changes may be lost. Continue?")]
[no-exit-message]
[private]
rebase-lint:
    git rebase -i `git merge-base HEAD upstream/master` \
    --exec "just lint" \

# Check each commit in the branch passes `just check`
[confirm("Warning, unsaved changes may be lost. Continue?")]
[no-exit-message]
[group('pr tools')]
prepare:
    git rebase -i `git merge-base HEAD upstream/master` \
    --exec "just check" \

# Git range-diff from <old-rev> to HEAD~ against master
[no-exit-message]
[group('tools')]
range-diff old-rev:
    git range-diff master {{ old-rev }} HEAD~

# Profile a running bitcoind for 60 seconds (e.g. just profile `pgrep bitcoind`). Outputs perf.data
[no-exit-message]
[group('perf')]
profile pid:
    perf record -g --call-graph dwarf --per-thread -F 140 -p {{ pid }} -- sleep 60

# Run benchmarks
[group('perf')]
bench:
    build/src/bench/bench_bitcoin

# Verify scripted diffs from master to HEAD~
[group('tools')]
verify-scripted-diff:
    test/lint/commit-script-check.sh origin/master..HEAD

# Install python deps from ci/lint/install.sh
[group('tools')]
install-python-deps:
    awk '/^\$\{CI_RETRY_EXE\} pip3 install \\/,/^$/{if (!/^\$\{CI_RETRY_EXE\} pip3 install \\/ && !/^$/) print}' ci/lint/04_install.sh \
        | sed 's/\\$//g' \
        | xargs pip3 install
    pip3 install vulture # currently unversioned in our repo
    pip3 install requests # only used in getcoins.py

deps_command := if os == "linux" { "xdg-open https://github.com/bitcoin/bitcoin/blob/master/doc/build-unix.md" } else { if os == "macos" { "open https://github.com/bitcoin/bitcoin/blob/master/doc/build-osx.md" } else { if os == "windows" { "explorer https://github.com/bitcoin/bitcoin/blob/master/doc/build-windows.md" } else { if os == "freebsd" { "xdg-open https://github.com/bitcoin/bitcoin/blob/master/doc/build-freebsd.md" } else { "echo see https://github.com/bitcoin/bitcoin/tree/master/doc#building for build instructions" } } } }

# Show project dependencies in browser
[group('tools')]
show-deps:
    {{ deps_command }}

# Build depends
[group('build')]
build-depends:
    #!/usr/bin/env bash
    set -euxo pipefail

    # Run make and capture its output while displaying it
    cd depends && make -j$(nproc) 2>&1 | tee /tmp/make_output.txt

    # Extract the line that starts with 'to: ' and ends with your host triplet
    build_path=$(grep -E '^to: .*/depends/[^/]+$' /tmp/make_output.txt | awk '{print $2}')

    # Check if build_path was found
    if [ -z "$build_path" ]; then
        echo "Error: Build may have completed successfully but host-triplet could not be automatically detected."
        exit 1
    fi

    # Extract the relative path starting from 'depends/'
    HOST_TRIPLET_PATH="depends/${build_path##*/depends/}"

    echo
    echo "To use depends, run cmake using the toolchain of the host triplet."
    echo "This was auto-detected on this run as:"
    echo
    echo "cmake -B build --toolchain $HOST_TRIPLET_PATH/toolchain.cmake"

# Attest to guix build outputs
[group('guix')]
guix-attest:
    contrib/guix/guix-attest

# Perform a guix build with default options
[group('guix')]
guix-build:
    contrib/guix/guix-build

# Clean intermediate guix build work directories
[group('guix')]
guix-clean:
    contrib/guix/guix-clean

# Codesign build outputs
[group('guix')]
guix-codesign:
    contrib/guix/guix-codesign

# Verify build output attestations
[group('guix')]
guix-verify:
    contrib/guix/guix-verify

# Run the CI workflow
[group('ci')]
run-ci: build-ci && bench
