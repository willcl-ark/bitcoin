#!/usr/bin/env bash
#
# Copyright (c) The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit.

set -o errexit -o nounset -o pipefail -o xtrace

WORKSPACE_PARENT="${GITHUB_WORKSPACE%/*}"

export BASE_ROOT_DIR="${GITHUB_WORKSPACE}"
export BASE_BUILD_DIR="${WORKSPACE_PARENT}/bsd-build"
export BASE_SCRATCH_DIR="${WORKSPACE_PARENT}/bsd-scratch"
export CCACHE_DIR="${GITHUB_WORKSPACE}/ci/scratch/ccache"
export DEPENDS_DIR="${GITHUB_WORKSPACE}/depends"
export BASE_CACHE="${DEPENDS_DIR}/built"
export SOURCES_PATH="${DEPENDS_DIR}/sources"
export PREVIOUS_RELEASES_DIR="${GITHUB_WORKSPACE}/releases"
export TMPDIR="${BASE_SCRATCH_DIR}/tmp"
export CCACHE_TEMPDIR="${BASE_SCRATCH_DIR}/ccache-temp"
export DANGER_RUN_CI_ON_HOST=1
export CI_FAILFAST_TEST_LEAVE_DANGLING=1
export CI_RETRY_EXE="${GITHUB_WORKSPACE}/ci/retry/retry"
export FILE_ENV="$1"

mkdir -p \
  "${CCACHE_DIR}" \
  "${PREVIOUS_RELEASES_DIR}" \
  "${TMPDIR}" \
  "${CCACHE_TEMPDIR}"

# shellcheck disable=SC1091
source "${BASE_ROOT_DIR}/ci/test/00_setup_env.sh"

# The native BSD vms currently cannot complete the reindex-init scenario: after
# reindexing is allowed, the node remains at height 0 instead of reaching 200.
export TEST_RUNNER_EXTRA="${TEST_RUNNER_EXTRA:-} --exclude feature_reindex_init"
# The native BSD vms struggle with adding peers during this test and often
# timeout after 40 minutes.
export TEST_RUNNER_EXTRA="${TEST_RUNNER_EXTRA} --exclude p2p_private_broadcast_retry_v1"

if [ -n "${CI_LIMIT_NOFILE:-}" ]; then
  ulimit -n "${CI_LIMIT_NOFILE}"
fi

"${BASE_ROOT_DIR}/ci/test/03_test_script.sh"
