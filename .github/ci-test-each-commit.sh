#!/usr/bin/env bash
#
# Copyright (c) The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

export LC_ALL=C.UTF-8

set -o errexit -o pipefail -o xtrace

if [ $# -ne 3 ]; then
    echo "Usage: $0 <base.sha> <head.sha> <MAX_COUNT>"
    exit 1
fi

BASE_SHA="$1"
HEAD_SHA="$2"
MAX_COUNT="$3"

git config user.email "ci@example.com"
git config user.name "CI"

git fetch "$HEAD_SHA" --depth=0
git switch -c pr "$HEAD_SHA"

git fetch origin master --depth=0
git checkout master
git merge --no-commit pr

MERGE_BASE=$(git merge-base "${BASE_SHA}" HEAD^1)
# Drop the top commit (last in the list) because it would be wasteful to rerun tests on the PR head commit that are already run by other jobs.
COMMITS=$(git rev-list --max-count="${MAX_COUNT}" --reverse "${MERGE_BASE}".."${HEAD_SHA}" | head -n -1)

if [ -z "$COMMITS" ]; then
  echo "All commits filtered out. This probably indicates an error in the merge base or commit list calculations"
  exit 1
fi

echo "Commits to be tested:"
echo "$COMMITS"

sudo apt-get update
sudo apt-get install -y clang ccache build-essential cmake ninja-build pkgconf python3-zmq libevent-dev libboost-dev libsqlite3-dev libdb++-dev systemtap-sdt-dev libzmq3-dev qt6-base-dev qt6-tools-dev qt6-l10n-tools libqrencode-dev

for COMMIT in $COMMITS; do
  echo "Testing commit $COMMIT"
  git checkout "$COMMIT" || { echo "Failed to checkout $COMMIT"; exit 1; }
  # Use clang++, because it is a bit faster and uses less memory than g++
  CC=clang CXX=clang++ cmake -B build -DWERROR=ON -DWITH_ZMQ=ON -DBUILD_GUI=ON -DBUILD_BENCH=ON -DBUILD_FUZZ_BINARY=ON -DWITH_BDB=ON -DWITH_USDT=ON -DCMAKE_CXX_FLAGS='-Wno-error=unused-member-function' || exit 1
  cmake --build build -j "$(nproc)" || exit 1
  ctest --output-on-failure --stop-on-failure --test-dir build -j "$(nproc)" || exit 1
  ./build/test/functional/test_runner.py -j $(( $(nproc) * 2 )) --combinedlogslen=99999999 || exit 1
  echo "Tests passed for commit $COMMIT"
  rm -rf build
done
