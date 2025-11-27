#!/usr/bin/env bash
#
# Copyright (c) 2018-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

export LC_ALL=C.UTF-8

set -ex

cd "${BASE_ROOT_DIR}"

export PATH="/path_with space:${PATH}"

echo "Number of available processing units: $(nproc)"
if [ "$CI_OS_NAME" == "macos" ]; then
  top -l 1 -s 0 | awk ' /PhysMem/ {print}'
else
  free -m -h
  echo "System info: $(uname --kernel-name --kernel-release)"
  lscpu
fi
echo "Free disk space:"
df -h

# We force an install of linux-headers again here via $PACKAGES to fix any
# kernel mismatch between a cached docker image and the underlying host.
# This can happen occasionally on hosted runners if the runner image is updated.
if [[ "$CONTAINER_NAME" == "ci_native_asan" ]]; then
  $CI_RETRY_EXE apt-get update
  ${CI_RETRY_EXE} bash -c "apt-get install --no-install-recommends --no-upgrade -y $PACKAGES"
fi

# What host to compile for. See also ./depends/README.md
# Tests that need cross-compilation export the appropriate HOST.
# Tests that run natively guess the host
export HOST=${HOST:-$("$BASE_ROOT_DIR/depends/config.guess")}

echo "=== BEGIN env ==="
env
echo "=== END env ==="

# Don't apply patches in the iwyu job, because it relies on the `git diff`
# command to detect IWYU errors. It is safe to skip this patch in the iwyu job
# because it doesn't run a UB detector.
if [[ "${RUN_IWYU}" != true ]]; then
  # compact->outputs[i].file_size is uninitialized memory, so reading it is UB.
  # The statistic bytes_written is only used for logging, which is disabled in
  # CI, so as a temporary minimal fix to work around UB and CI failures, leave
  # bytes_written unmodified.
  # See https://github.com/bitcoin/bitcoin/pull/28359#issuecomment-1698694748
  # Tee patch to stdout to make it clear CI is testing modified code.
  tee >(patch -p1) <<'EOF'
--- a/src/leveldb/db/db_impl.cc
+++ b/src/leveldb/db/db_impl.cc
@@ -1028,9 +1028,6 @@ Status DBImpl::DoCompactionWork(CompactionState* compact) {
       stats.bytes_read += compact->compaction->input(which, i)->file_size;
     }
   }
-  for (size_t i = 0; i < compact->outputs.size(); i++) {
-    stats.bytes_written += compact->outputs[i].file_size;
-  }

   mutex_.Lock();
   stats_[compact->compaction->level() + 1].Add(stats);
EOF
fi

if [ "$RUN_FUZZ_TESTS" = "true" ]; then
  export DIR_FUZZ_IN=${DIR_QA_ASSETS}/fuzz_corpora/
  if [ ! -d "$DIR_FUZZ_IN" ]; then
    ${CI_RETRY_EXE} git clone --depth=1 https://github.com/bitcoin-core/qa-assets "${DIR_QA_ASSETS}"
  fi
  (
    cd "${DIR_QA_ASSETS}"
    echo "Using qa-assets repo from commit ..."
    git log -1
  )
elif [ "$RUN_UNIT_TESTS" = "true" ]; then
  export DIR_UNIT_TEST_DATA=${DIR_QA_ASSETS}/unit_test_data/
  if [ ! -d "$DIR_UNIT_TEST_DATA" ]; then
    mkdir -p "$DIR_UNIT_TEST_DATA"
    ${CI_RETRY_EXE} curl --location --fail https://github.com/bitcoin-core/qa-assets/raw/main/unit_test_data/script_assets_test.json -o "${DIR_UNIT_TEST_DATA}/script_assets_test.json"
  fi
fi

# Make sure default datadir does not exist and is never read by creating a dummy file
if [ "$CI_OS_NAME" == "macos" ]; then
  echo > "${HOME}/Library/Application Support/Bitcoin"
else
  echo > "${HOME}/.bitcoin"
fi

if [ -z "$NO_DEPENDS" ]; then
  if [[ $CI_IMAGE_NAME_TAG == *alpine* ]]; then
    SHELL_OPTS="CONFIG_SHELL=/usr/bin/dash"
  else
    SHELL_OPTS="CONFIG_SHELL="
  fi
  bash -c "$SHELL_OPTS make $MAKEJOBS -C depends HOST=$HOST $DEP_OPTS LOG=1"
fi
if [ "$DOWNLOAD_PREVIOUS_RELEASES" = "true" ]; then
  test/get_previous_releases.py --target-dir "$PREVIOUS_RELEASES_DIR"
fi

ccache --zero-stats
PRINT_CCACHE_STATISTICS="ccache --version | head -n 1 && ccache --show-stats"

# Folder where the build is done.
BASE_BUILD_DIR=${BASE_BUILD_DIR:-$BASE_SCRATCH_DIR/build-$HOST}

cmake -S "$BASE_ROOT_DIR" -B "$BASE_BUILD_DIR" --preset "$CONTAINER_NAME" -DCMAKE_INSTALL_PREFIX="$BASE_OUTDIR" || (
  cd "${BASE_BUILD_DIR}"
  # shellcheck disable=SC2046
  cat $(cmake -P "${BASE_ROOT_DIR}/ci/test/GetCMakeLogFiles.cmake")
  false
)

if [[ "${GOAL}" != all && "${GOAL}" != codegen ]]; then
  GOAL="all ${GOAL}"
fi

# shellcheck disable=SC2086
cmake --build "${BASE_BUILD_DIR}" "$MAKEJOBS" || (
  echo "Build failure. Verbose build follows."
  # shellcheck disable=SC2086
  cmake --build "${BASE_BUILD_DIR}" -j1 --verbose
  false
)

bash -c "${PRINT_CCACHE_STATISTICS}"
if [ "$CI" = "true" ]; then
  hit_rate=$(ccache -s | grep "Hits:" | head -1 | sed 's/.*(\(.*\)%).*/\1/')
  if [ "${hit_rate%.*}" -lt 75 ]; then
      echo "::notice title=low ccache hitrate::Ccache hit-rate in $CONTAINER_NAME was $hit_rate%"
  fi
fi
du -sh "${DEPENDS_DIR}"/*/
du -sh "${PREVIOUS_RELEASES_DIR}"

if [ -n "${CI_LIMIT_STACK_SIZE}" ]; then
  ulimit -s 512
fi

if [ -n "$USE_VALGRIND" ]; then
  "${BASE_ROOT_DIR}/ci/test/wrap-valgrind.sh"
fi

# Run tests using CMake preset
DIR_UNIT_TEST_DATA="${DIR_UNIT_TEST_DATA}" \
LD_LIBRARY_PATH="${DEPENDS_DIR}/${HOST}/lib" \
CTEST_OUTPUT_ON_FAILURE=ON \
ctest --preset "${CONTAINER_NAME}" --test-dir "${BASE_BUILD_DIR}" "${MAKEJOBS}" --timeout $(( TEST_RUNNER_TIMEOUT_FACTOR * 60 ))
