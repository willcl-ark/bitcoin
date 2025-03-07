#!/usr/bin/env bash
#
# Runner script for Bitcoin CI in Docker
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

# Config and docker directories
CONFIG_DIR="${SCRIPT_DIR}/../config"
DOCKER_DIR="${SCRIPT_DIR}/../docker"

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

# Source the job configuration to get the variables for this script
# We don't export them as they'll be passed to Docker via the .env file
# shellcheck disable=SC1090
source "${CONFIG_FILE}"

# Check if Docker is available
if ! command_exists docker; then
    log_error "Docker is not installed or not in PATH"
    exit 1
fi

# Check if docker buildx is available
if ! docker buildx version &>/dev/null; then
    log_error "Docker Buildx plugin is not installed"
    log_info "You can install it following the instructions at:"
    log_info "https://docs.docker.com/buildx/working-with-buildx/"
    exit 1
fi

# Check if image exists or build it
log_info "Checking if image bitcoin-ci:${JOB_NAME} exists..."
if ! docker image inspect "bitcoin-ci:${JOB_NAME}" &>/dev/null; then
    log_info "Building image for ${JOB_NAME}..."
    if [[ ! -f "${DOCKER_DIR}/docker-bake.hcl" ]]; then
        log_error "Docker bake file not found: ${DOCKER_DIR}/docker-bake.hcl"
        exit 1
    fi

    # Build the image using the .env file
    docker buildx bake --file "${DOCKER_DIR}/docker-bake.hcl" --file "${CONFIG_FILE}" "${JOB_NAME}"

    # Check if the build was successful
    if ! docker image inspect "bitcoin-ci:${JOB_NAME}" &>/dev/null; then
        log_error "Failed to build image: bitcoin-ci:${JOB_NAME}"
        exit 1
    fi
fi

# Create volumes if they don't exist
log_info "Creating Docker volumes..."
docker volume create "bitcoin-ci-${JOB_NAME}-ccache" &>/dev/null || true
docker volume create "bitcoin-ci-${JOB_NAME}-depends" &>/dev/null || true
docker volume create "bitcoin-ci-${JOB_NAME}-depends-sources" &>/dev/null || true
docker volume create "bitcoin-ci-${JOB_NAME}-previous-releases" &>/dev/null || true

# Get source directory (current directory)
SRC_DIR="$(pwd)"
log_info "Source directory: ${SRC_DIR}"

# Run the container
log_info "Running container..."
docker run --rm -it \
    --platform="${CI_IMAGE_PLATFORM:-linux/amd64}" \
    -v "${SRC_DIR}:/bitcoin:ro" \
    -v "bitcoin-ci-${JOB_NAME}-ccache:/ci_container_base/ci/scratch/ccache" \
    -v "bitcoin-ci-${JOB_NAME}-depends:/ci_container_base/depends/built" \
    -v "bitcoin-ci-${JOB_NAME}-depends-sources:/ci_container_base/depends/sources" \
    -v "bitcoin-ci-${JOB_NAME}-previous-releases:/ci_container_base/prev_releases" \
    --name "${CONTAINER_NAME:-bitcoin-ci-${JOB_NAME}}" \
    "bitcoin-ci:${JOB_NAME}" \
    bash -c "cd /bitcoin && ./ci/test/03_test_script.sh ${EXTRA_ARGS}"

log_info "Job ${JOB_NAME} completed successfully"
