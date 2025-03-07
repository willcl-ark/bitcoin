#!/usr/bin/env bash
#
# Runner script for Bitcoin CI on bare metal
#
# Copyright (c) 2025-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

set -eo pipefail

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
# shellcheck disable=SC1091
source "${SCRIPT_DIR}/common.sh"

if [[ $# -lt 1 ]]; then
    log_error "Job name not specified"
    exit 1
fi

JOB_NAME="$1"
shift
EXTRA_ARGS="$*"

# Config directory
CONFIG_DIR="${SCRIPT_DIR}/../config"

# Check if job configuration exists
CONFIG_FILE="${CONFIG_DIR}/${JOB_NAME}.env"
if [[ ! -f "${CONFIG_FILE}" ]]; then
    log_error "Job configuration not found: ${CONFIG_FILE}"
    log_info "Available jobs:"
    for config_file in "${CONFIG_DIR}"/*.env; do
        if [[ -f "${config_file}" ]]; then
            basename "${config_file%.env}"
        fi
    done
    exit 1
fi

log_info "Running job ${JOB_NAME} on host system"

# Source the job-specific environment variables
# shellcheck disable=SC1090
source "${CONFIG_FILE}"

# Create directories
REPO_ROOT="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
BASE_ROOT_DIR="${REPO_ROOT}"
CCACHE_DIR="${BASE_ROOT_DIR}/ci/scratch/ccache"
DEPENDS_DIR="${BASE_ROOT_DIR}/depends"
PREVIOUS_RELEASES_DIR="${BASE_ROOT_DIR}/prev_releases"
BINS_SCRATCH_DIR="${BASE_ROOT_DIR}/ci/scratch/bins"
BASE_SCRATCH_DIR="${BASE_ROOT_DIR}/ci/scratch"
BASE_OUTDIR="${BASE_ROOT_DIR}/ci/scratch/out"

log_info "Creating directories..."
mkdir -p "${CCACHE_DIR}"
mkdir -p "${DEPENDS_DIR}/built"
mkdir -p "${DEPENDS_DIR}/sources"
mkdir -p "${PREVIOUS_RELEASES_DIR}"
mkdir -p "${BINS_SCRATCH_DIR}"
mkdir -p "${BASE_OUTDIR}"

# Export all environment variables from the config file
# This makes them available to the test script
log_info "Setting environment variables..."
while IFS='=' read -r key value || [[ -n "$key" ]]; do
    # Skip comments and empty lines
    [[ "$key" =~ ^#.*$ || -z "$key" ]] && continue

    # Remove quotes if present
    value=${value//\"/}
    value=${value//\'/}

    log_info "  $key=$value"
    export "$key"="$value"
done < "${CONFIG_FILE}"

# Set the environment variables for running on host
export DANGER_RUN_CI_ON_HOST=1
export BASE_ROOT_DIR="${BASE_ROOT_DIR}"
export CCACHE_DIR="${CCACHE_DIR}"
export DEPENDS_DIR="${DEPENDS_DIR}"
export PREVIOUS_RELEASES_DIR="${PREVIOUS_RELEASES_DIR}"
export BINS_SCRATCH_DIR="${BINS_SCRATCH_DIR}"
export BASE_SCRATCH_DIR="${BASE_SCRATCH_DIR}"
export BASE_OUTDIR="${BASE_OUTDIR}"

# Create CI_EXEC function (compatible with the existing script)
CI_EXEC() {
  bash -c "export PATH=\"${BINS_SCRATCH_DIR}:${BASE_ROOT_DIR}/ci/retry:\$PATH\" && cd \"${BASE_ROOT_DIR}\" && $*"
}
export -f CI_EXEC

log_info "Running test script..."
"${BASE_ROOT_DIR}/ci/test/03_test_script.sh" ${EXTRA_ARGS}

log_info "Job ${JOB_NAME} completed successfully"
