#!/usr/bin/env bash
#
# Copyright (c) 2018-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

export LC_ALL=C

set -o errexit -o pipefail -o xtrace

# Fixes permission issues when there is a container UID/GID mismatch with the owner
# of the mounted bitcoin src dir.
git config --global --add safe.directory /bitcoin

export PATH="/python_env/bin:${PATH}"
export MYPY_CACHE_DIR="/tmp/mypy_cache"
export PYTHONDONTWRITEBYTECODE=1

if [ -n "${LINT_CI_IS_PR}" ]; then
  export COMMIT_RANGE="HEAD~..HEAD"
  if [ "$(git rev-list -1 HEAD)" != "$(git rev-list -1 --merges HEAD)" ]; then
    echo "Error: The top commit must be a merge commit, usually the remote 'pull/<PR_NUMBER>/merge' branch."
    false
  fi
fi

RUST_BACKTRACE=1 lint_test_runner "$@"

if [ "${LINT_CI_SANITY_CHECK_COMMIT_SIG}" = "1" ] ; then
    # Signature verification updates trusted roots and Git metadata.
    SOURCE_HEAD="$(git rev-parse HEAD)"
    VERIFY_COMMITS_DIR="$(mktemp -d)"
    trap 'rm -rf "${VERIFY_COMMITS_DIR}"' EXIT
    git clone --quiet --shared --no-checkout /bitcoin "${VERIFY_COMMITS_DIR}"
    git -C "${VERIFY_COMMITS_DIR}" checkout --quiet --detach "${SOURCE_HEAD}"
    cd "${VERIFY_COMMITS_DIR}" || exit 1

    # Sanity check only the last few commits to get notified of missing sigs,
    # missing keys, or expired keys. Usually there is only one new merge commit
    # per push on the master branch and a few commits on release branches, so
    # sanity checking only a few (10) commits seems sufficient and cheap.
    git log HEAD~10 -1 --format='%H' > ./contrib/verify-commits/trusted-sha512-root-commit
    git log HEAD~10 -1 --format='%H' > ./contrib/verify-commits/trusted-git-root
    mapfile -t KEYS < contrib/verify-commits/trusted-keys
    git config user.email "ci@ci.ci"
    git config user.name "ci"
    ${CI_RETRY_EXE} gpg --keyserver hkps://keys.openpgp.org --recv-keys "${KEYS[@]}" &&
    ./contrib/verify-commits/verify-commits.py;
fi
