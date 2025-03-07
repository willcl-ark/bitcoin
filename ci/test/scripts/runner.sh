#!/usr/bin/env bash
#
# Main runner script for Bitcoin CI
#
# Copyright (c) 2025-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

set -eo pipefail

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
# shellcheck disable=SC1091
source "${SCRIPT_DIR}/common.sh"

# Parse arguments
JOB_NAME=""
HOST_MODE=false
EXTRA_ARGS=""

print_usage() {
    echo "Usage: $0 --job JOB_NAME [--host] [-- EXTRA_ARGS]"
    echo "  --job JOB_NAME   Specify the job to run (e.g., arm, mac_cross)"
    echo "  --host           Run on host system (not in Docker)"
    echo "  -- EXTRA_ARGS    Pass additional arguments to the job"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --job)
            if [[ $# -lt 2 ]]; then
                log_error "Missing argument for --job"
                print_usage
                exit 1
            fi
            JOB_NAME="$2"
            shift 2
            ;;
        --host)
            HOST_MODE=true
            shift
            ;;
        --)
            shift
            EXTRA_ARGS="$*"
            break
            ;;
        -h|--help)
            print_usage
            exit 0
            ;;
        *)
            log_error "Unknown option: $1"
            print_usage
            exit 1
            ;;
    esac
done

if [[ -z "$JOB_NAME" ]]; then
    log_error "Job name not specified"
    print_usage
    exit 1
fi

# Check requirements
if ! check_requirements; then
    exit 1
fi

# Config directory
CONFIG_DIR="${SCRIPT_DIR}/../config"

# Check if job configuration exists
if ! job_config_exists "$JOB_NAME" "$CONFIG_DIR"; then
    log_error "Job configuration not found for: $JOB_NAME"
    log_info "Available jobs:"
    for config_file in "$CONFIG_DIR"/*.env; do
        if [[ -f "$config_file" ]]; then
            basename "${config_file%.env}"
        fi
    done
    exit 1
fi

log_info "Running job: $JOB_NAME"
log_info "Host mode: $HOST_MODE"
if [[ -n "$EXTRA_ARGS" ]]; then
    log_info "Extra arguments: $EXTRA_ARGS"
fi

# Set up environment
if [[ "$HOST_MODE" == true ]]; then
    exec "${SCRIPT_DIR}/host-runner.sh" "$JOB_NAME" $EXTRA_ARGS
else
    exec "${SCRIPT_DIR}/docker-runner.sh" "$JOB_NAME" $EXTRA_ARGS
fi
